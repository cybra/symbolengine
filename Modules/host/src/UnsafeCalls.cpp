#include "UnsafeCalls.h"

#include "symbolengine/core/Paths.h"

#include <algorithm>
#include <mutex>
#include <string>
#include <system_error>

#if defined(_WIN32)
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#else
    #include <cxxabi.h>
    #include <dlfcn.h>
    #include <execinfo.h>
    #include <pthread.h>

    #include <cstdlib>
    #if defined(__APPLE__)
        #include <mach-o/dyld.h>
    #endif
#endif

namespace symbolengine
{

namespace unsafeCalls
{

namespace
{

// As many frames as tell one place from another, however deep the plugin's own code goes below them, so finding a place
// again costs little where a plugin makes many such calls.
constexpr int framesTold = 12;

std::uint64_t mixed(std::uint64_t hash, const void* value)
{
    return (hash ^ reinterpret_cast<std::uintptr_t>(value)) * 0x100000001B3ull;
}

// A path as it really is, for comparing with what the system says of a module.
std::string compared(const std::filesystem::path& path)
{
    // Nothing stays nothing, which the system would take for the current folder.
    if (path.empty())
        return {};

    std::error_code ignored;
    const auto real = std::filesystem::weakly_canonical(path, ignored);
    return paths::toUtf8(real.empty() ? path : real);
}

bool within(const std::string& path, const std::string& folder)
{
    if (folder.empty() || path.compare(0, folder.size(), folder) != 0)
        return false;

    return path.size() == folder.size() || folder.back() == '/' || path[folder.size()] == '/';
}

}

Record::Record(const Link& linked, std::filesystem::path pluginFile, Told toldNew, void* toldContext)
    : link(linked), plugin(std::move(pluginFile)), told(toldNew), context(toldContext)
{
}

void Record::enter(const char* calledStep, std::int64_t calledBlock) noexcept
{
    step = calledStep;
    block = calledBlock;
    link.enter(this);
}

void Record::leave() noexcept
{
    link.leave();
}

void Record::saw(void* watched, const char* function, int kind, void* returnAddress)
{
    static_cast<Record*>(watched)->found(function, kind, returnAddress);
}

void Record::found(const char* function, int kind, void* returnAddress) noexcept
{
    // Where the call came from starts past what sees it, which an inlined function can make a frame more or less of.
    void* frames[framesTold + 8];
    const int count = captureFrames(frames, framesTold + 8);
    const auto* const end = frames + count;
    const auto* const from = std::find(static_cast<void**>(frames), frames + count, returnAddress);
    const auto* const start = from == end ? frames : from;

    auto hash = mixed(mixed(0xCBF29CE484222325ull, function), step);

    for (const auto* frame = start; frame != end && frame != start + framesTold; ++frame)
        hash = mixed(hash, *frame);

    // Never 0, which says a place is free.
    const auto key = hash | 1u;

    for (std::size_t probe = 0; probe < kept.size(); ++probe)
    {
        auto& place = kept[(key + probe) % kept.size()];
        const auto there = place.key.load(std::memory_order_acquire);

        if (there != 0 && there != key)
            continue;

        if (there == 0)
        {
            place.function = function;
            place.step = step;
            place.kind = kind;
            place.order = nextOrder++;
            place.firstBlock = block;
            void* whole[framesKept + 8];
            const int depth = captureFrames(whole, framesKept + 8);
            const auto* const wholeEnd = whole + depth;
            const auto* const wholeFrom = std::find(static_cast<void**>(whole), whole + depth, returnAddress);
            const auto* const wholeStart = wholeFrom == wholeEnd ? whole : wholeFrom;
            place.frameCount = std::min(static_cast<int>(wholeEnd - wholeStart), framesKept);
            std::copy_n(wholeStart, place.frameCount, place.frames.begin());
            place.times.store(1, std::memory_order_relaxed);
            place.blocks.store(1, std::memory_order_relaxed);
            place.lastBlock.store(block, std::memory_order_relaxed);
            place.key.store(key, std::memory_order_release);

            if (told != nullptr)
                told(context);

            return;
        }

        place.times.fetch_add(1, std::memory_order_relaxed);

        if (place.lastBlock.load(std::memory_order_relaxed) != block)
        {
            place.lastBlock.store(block, std::memory_order_relaxed);
            place.blocks.fetch_add(1, std::memory_order_relaxed);
        }

        return;
    }

    unlisted.fetch_add(1, std::memory_order_relaxed);
}

std::vector<PluginUnsafeCall> Record::calls() const
{
    std::vector<const Place*> found;

    for (const auto& place : kept)
        if (place.key.load(std::memory_order_acquire) != 0)
            found.push_back(&place);

    std::sort(found.begin(), found.end(), [](const auto* a, const auto* b) { return a->order < b->order; });

    const auto program = compared(programModule());
    const auto bundle = compared(plugin);
    std::vector<PluginUnsafeCall> calls;

    for (const auto* const place : found)
    {
        PluginUnsafeCall call;
        call.kind = static_cast<PluginUnsafeCall::Kind>(place->kind);
        call.function = place->function;
        call.step = place->step;
        call.times = place->times.load(std::memory_order_relaxed);
        call.blocks = place->blocks.load(std::memory_order_relaxed);
        call.firstBlock = place->firstBlock;
        call.lastBlock = place->lastBlock.load(std::memory_order_relaxed);

        std::size_t pastPlugin = 0;

        for (int f = 0; f < place->frameCount; ++f)
        {
            auto frame = describeFrame(place->frames[static_cast<std::size_t>(f)]);
            const auto module = frame.module.empty() ? std::string{} : compared(frame.module);
            call.backtrace.push_back(std::move(frame.text));

            // The plugin's own file first, as a plugin may be installed among the system's own folders; then anything
            // that is neither the program nor the system's is taken for a library the plugin brought.
            if (!module.empty() && (within(module, bundle) || (module != program && !isSystemModule(frame.module))))
            {
                call.inPluginCode = true;
                pastPlugin = call.backtrace.size();
            }
        }

        // What lies past the plugin's own code is the same for every call: the host's way into it, the thread's start.
        if (call.inPluginCode)
            call.backtrace.resize(pastPlugin);

        calls.push_back(std::move(call));
    }

    return calls;
}

#if !defined(_WIN32)

void tryCalls()
{
    void* volatile memory = std::malloc(16);
    std::free(memory);
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock);
    pthread_mutex_unlock(&lock);
}

const Link* link()
{
    using Find = const Link* (*)();
    const auto find = reinterpret_cast<Find>(dlsym(RTLD_DEFAULT, "symbolengine_unsafe_calls_link"));
    return find != nullptr ? find() : nullptr;
}

int captureFrames(void** frames, int most) noexcept
{
    return backtrace(frames, most);
}

Frame describeFrame(void* frame)
{
    Dl_info info{};

    // A return address, which may be just past the function it is in, so the look is a byte before it.
    if (dladdr(static_cast<char*>(frame) - 1, &info) == 0 || info.dli_fname == nullptr)
        return {{}, "?"};

    Frame described{info.dli_fname, info.dli_fname};

    if (info.dli_sname != nullptr)
    {
        int status = 0;
        char* const plain = abi::__cxa_demangle(info.dli_sname, nullptr, nullptr, &status);
        described.text += ": ";
        described.text += status == 0 && plain != nullptr ? plain : info.dli_sname;
        std::free(plain);
    }

    return described;
}

bool isSystemModule(const std::filesystem::path& module)
{
    const auto path = paths::toUtf8(module);
    #if defined(__APPLE__)
    return within(path, "/usr/lib/") || within(path, "/System/");
    #else
    return within(path, "/lib/") || within(path, "/lib64/") || within(path, "/usr/lib/") || within(path, "/usr/lib64/");
    #endif
}

std::filesystem::path programModule()
{
    Dl_info info{};

    // The module holding the host, which is the program itself unless it keeps the host in a library of its own.
    if (dladdr(reinterpret_cast<const void*>(&programModule), &info) != 0 && info.dli_fname != nullptr)
        return info.dli_fname;

    return {};
}

#endif

}

}
