#include "symbolengine/host/PluginCrashNote.h"

#include "CrashNoteRecords.h"

#include <symbolengine/core/Files.h>
#include <symbolengine/core/Paths.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <new>
#include <string>

namespace symbolengine
{

namespace
{

// Text copied into a fixed field, cut to fit and always ended.
template <std::size_t size>
void put(char (&field)[size], std::string_view text) noexcept
{
    const auto length = std::min(text.size(), size - 1);
    std::memcpy(field, text.data(), length);
    field[length] = '\0';
}

// A field of the note set whole, after everything before it, as the next run or a crash may read it at any moment.
template <typename Field>
void set(Field& field, Field value) noexcept
{
    std::atomic_thread_fence(std::memory_order_release);
    *static_cast<volatile Field*>(&field) = value;
}

template <std::size_t size>
std::string read(const char (&field)[size])
{
    return std::string(field, strnlen(field, size));
}

std::uint64_t ownerOf(const crashNote::Thread& thread) noexcept
{
    return std::atomic_ref(const_cast<std::uint64_t&>(thread.owner)).load(std::memory_order_acquire);
}

bool takeOver(crashNote::Thread& thread, std::uint64_t from, std::uint64_t to) noexcept
{
    return std::atomic_ref(thread.owner).compare_exchange_strong(from, to, std::memory_order_acq_rel);
}
}

struct PluginCrashNote::Impl
{
    PluginRunEnding last;
    crashNote::Layout* layout = nullptr;
    void* handle = nullptr;
    bool catching = false;
    bool inMemoryOnly = false;
    std::mutex slots;

    // The calling thread's record, found by the thread's own number. Nothing here is kept per thread by the system, whose
    // storage for that can be made on first use, which an audio thread's first call or a crash's handler mustn't cause.
    // A record whose thread is in no call can be taken over, so threads that have ended leave room for those that come.
    crashNote::Thread* threadRecord(std::uint64_t self, bool& fresh) noexcept
    {
        fresh = false;
        for (auto& thread : layout->threads)
            if (ownerOf(thread) == self)
                return &thread;
        fresh = true;
        for (auto& thread : layout->threads)
            if (ownerOf(thread) == 0 && takeOver(thread, 0, self))
                return &thread;
        for (auto& thread : layout->threads)
            if (const auto owner = ownerOf(thread); thread.slot < 0 && takeOver(thread, owner, self))
                return &thread;
        return nullptr;
    }
};

int crashNote::threadRecordIn(const Layout* layout) noexcept
{
    const auto self = native::thisThread();
    for (int index = 0; index < threadCount; ++index)
        if (ownerOf(layout->threads[index]) == self)
            return index;
    return -1;
}

const PluginRunEnding::Inside* PluginRunEnding::culprit() const
{
    if (clean)
        return nullptr;

    for (const auto& call : inside)
        if (call.crashed)
            return &call;

    // A crash was caught, and not in a call into a plugin, whatever the others were doing at the time.
    if (!crash.empty())
        return nullptr;

    for (const auto& call : inside)
        if (!call.audioThread)
            return &call;

    return inside.empty() ? nullptr : &inside.front();
}

namespace
{

// What a note left by the last run says, when it is one of ours whole: nothing known of a file cut short or of
// another kind.
PluginRunEnding readEnding(const std::string& bytes)
{
    PluginRunEnding ending;

    if (bytes.size() < sizeof(crashNote::Layout))
        return ending;

    const auto layout = std::make_unique<crashNote::Layout>();
    std::memcpy(layout.get(), bytes.data(), sizeof(crashNote::Layout));

    if (std::memcmp(layout->magic, crashNote::magic, sizeof crashNote::magic) != 0 ||
        layout->version != crashNote::version || layout->state != crashNote::running)
        return ending;

    ending.clean = false;
    ending.context = read(layout->context);
    ending.crash = std::string(layout->crash, std::min<std::size_t>(layout->crashLength, crashNote::crashRoom));

    for (const auto& thread : layout->threads)
    {
        if (thread.slot < 0 || thread.slot >= crashNote::slotCount || layout->slots[thread.slot].used == 0)
            continue;

        const auto& slot = layout->slots[thread.slot];
        PluginRunEnding::Inside call;
        call.plugin.identity = {read(slot.format), read(slot.id)};
        call.plugin.name = read(slot.name);
        call.plugin.maker = read(slot.maker);
        call.plugin.location = paths::fromUtf8(read(slot.location));
        call.call = read(thread.call);
        call.audioThread = thread.audio != 0;
        call.crashed = thread.crashed != 0;
        ending.inside.push_back(std::move(call));
    }

    return ending;
}

}

std::unique_ptr<PluginCrashNote> PluginCrashNote::open(const std::filesystem::path& file, bool catchCrashes,
                                                       PluginError& error)
{
    error = {};
    auto made = std::make_unique<Impl>();
    std::string before;
    bool anotherCopy = false;
    made->layout = crashNote::native::map(file, made->handle, before, anotherCopy);

    // Another copy of the program has the file. Its note isn't this one's to read, as running it looks like one ended by
    // a crash, or to change, so this copy goes without one to leave.
    if (anotherCopy)
    {
        made->layout = new (std::nothrow) crashNote::Layout;
        made->inMemoryOnly = true;
        catchCrashes = false;
    }
    else
    {
        made->last = readEnding(before);
    }

    if (made->layout == nullptr)
    {
        error = {"The crash note can't be made at " + paths::toUtf8(file) + ".", "open", 0};
        return nullptr;
    }

    auto& layout = *made->layout;
    std::memset(&layout, 0, sizeof layout);
    for (auto& thread : layout.threads)
        thread.slot = -1;
    std::memcpy(layout.magic, crashNote::magic, sizeof crashNote::magic);
    layout.version = crashNote::version;
    set(layout.state, crashNote::running);

    if (catchCrashes)
    {
        crashNote::native::catchCrashes(made->layout, made->handle);
        made->catching = true;
    }

    return std::unique_ptr<PluginCrashNote>(new PluginCrashNote(std::move(made)));
}

PluginCrashNote::PluginCrashNote(std::unique_ptr<Impl> made) : impl(std::move(made)) {}

PluginCrashNote::~PluginCrashNote()
{
    if (impl->catching)
        crashNote::native::stopCatching();

    set(impl->layout->state, crashNote::ended);
    if (impl->inMemoryOnly)
        delete impl->layout;
    else
        crashNote::native::unmap(impl->layout, impl->handle);
}

const PluginRunEnding& PluginCrashNote::lastRun() const
{
    return impl->last;
}

void PluginCrashNote::setContext(std::string_view context)
{
    put(impl->layout->context, context.substr(0, 1000));
}

int crashNote::claim(PluginCrashNote* note, const PluginDescription& plugin)
{
    if (note == nullptr)
        return -1;

    const std::lock_guard held(note->impl->slots);
    auto& slots = note->impl->layout->slots;

    for (int index = 0; index < slotCount; ++index)
        if (slots[index].used == 0)
        {
            auto& slot = slots[index];
            put(slot.format, plugin.identity.format);
            put(slot.id, plugin.identity.id);
            put(slot.name, plugin.name);
            put(slot.maker, plugin.maker);
            put(slot.location, paths::toUtf8(plugin.location));
            set(slot.used, std::uint32_t{1});
            return index;
        }

    return -1;
}

void crashNote::release(PluginCrashNote* note, int slot)
{
    if (note == nullptr || slot < 0 || slot >= slotCount)
        return;

    const std::lock_guard held(note->impl->slots);
    set(note->impl->layout->slots[slot].used, std::uint32_t{0});
}

crashNote::InCall::InCall(PluginCrashNote* note, int slot, const char* call, bool audioThread) noexcept
{
    if (note == nullptr || slot < 0)
        return;

    const auto self = native::thisThread();

    // Its record can be taken over just as it comes to use it, having been idle until then, which it sees after saying what
    // it is in, and takes back. Whoever took it has it, and nothing is left saying a call is under way that isn't.
    for (int tries = 0; tries < 3; ++tries)
    {
        bool fresh = false;
        auto* const found = note->impl->threadRecord(self, fresh);
        if (found == nullptr)
            return;

        // The call before it, of a call made from inside it, put back as this one ends.
        previousSlot = fresh ? -1 : found->slot;
        previousAudio = found->audio;
        std::memcpy(previousCall, found->call, sizeof previousCall);
        put(found->call, call);
        found->audio = audioThread ? 1 : 0;
        set(found->slot, static_cast<std::int32_t>(slot));

        if (ownerOf(*found) == self)
        {
            record = found;
            return;
        }

        auto said = static_cast<std::int32_t>(slot);
        std::atomic_ref(found->slot).compare_exchange_strong(said, -1);
    }
}

crashNote::InCall::~InCall()
{
    if (record == nullptr)
        return;

    if (previousSlot >= 0)
    {
        std::memcpy(record->call, previousCall, sizeof previousCall);
        record->audio = previousAudio;
    }

    set(record->slot, previousSlot);
}

}
