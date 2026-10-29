// The scan tests' bundles: one module copied under many names, each name saying what it does as its code loads or as
// one of its plugins is made, as "Crash on load.vst3" or "Many 40, 20 crashes, 50 ms.vst3". Its plugins pass their
// input through, stereo, and declare themselves as effects.

#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/vstspeaker.h"
#include "public.sdk/source/vst/vstaudioeffect.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <clocale>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <intrin.h>
    #include <windows.h>
#else
    #include <dlfcn.h>
#endif

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace
{

// Something of this module's own, whose address says which file it was loaded from.
const char inThisModule = 0;

// The bundle this code was loaded from, by its name without ".vst3", lower case.
std::string bundleName()
{
#if defined(_WIN32)
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&inThisModule), &module);
    std::vector<wchar_t> path(32768);
    GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    const std::filesystem::path binary(path.data());
#else
    Dl_info info{};
    dladdr(&inThisModule, &info);
    const std::filesystem::path binary(info.dli_fname != nullptr ? info.dli_fname : "");
#endif
    // Name.vst3/Contents/<architecture or MacOS>/binary
    auto name = binary.parent_path().parent_path().parent_path().stem().string();
    std::transform(name.begin(), name.end(), name.begin(),
                   [](char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c); });
    return name;
}

const std::string& name()
{
    static const std::string named = bundleName();
    return named;
}

bool says(const char* words)
{
    return name().find(words) != std::string::npos;
}

// The number written just before the words, as 20 in "20 crashes", or 0.
int numberBefore(const char* words)
{
    const auto at = name().find(words);
    if (at == std::string::npos || at == 0)
        return 0;
    auto start = at;
    while (start > 0 && name()[start - 1] == ' ')
        --start;
    auto end = start;
    while (start > 0 && name()[start - 1] >= '0' && name()[start - 1] <= '9')
        --start;
    return start == end ? 0 : std::atoi(name().substr(start, end - start).c_str());
}

// The number written just after the words, as 40 in "many 40", or 0.
int numberAfter(const char* words)
{
    const auto at = name().find(words);
    return at == std::string::npos ? 0 : std::atoi(name().c_str() + at + std::strlen(words));
}

[[noreturn]] void hang()
{
    for (;;)
        std::this_thread::sleep_for(std::chrono::seconds(1));
}

void crash()
{
    volatile int* nowhere = nullptr;
    *nowhere = 1;
}

// A plugin's own abort, as a release build's: on Windows with no message or report, which would wait for a person.
[[noreturn]] void abortHere()
{
#if defined(_WIN32)
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    std::abort();
}

volatile int deeper = 1;

int overflow(int depth)
{
    volatile char page[4096];
    page[0] = static_cast<char>(depth);
    return deeper != 0 ? overflow(depth + 1) + page[0] : page[0];
}

// What the bundle does as its code is loaded, before the host has called anything of it.
struct OnLoad
{
    std::mutex held;

    OnLoad()
    {
        if (says("crash on load"))
            crash();
        if (says("abort on load"))
            abortHere();
        if (says("hang on a thread on load"))
            std::thread(hang).join();
        else if (says("hang on load"))
            hang();

        // A thread of its own holding a lock that its code waits on as it is unloaded, which a process that ends
        // normally never gets past.
        if (says("detach deadlock"))
            std::thread([this] {
                held.lock();
                hang();
            }).detach();
    }

    ~OnLoad()
    {
        if (says("detach deadlock"))
            held.lock();
    }
};

OnLoad onLoad;

// What the plugin of this number does as it is made.
void onMake(int number)
{
    if (const int ms = numberBefore(" ms"); ms > 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    if (numberBefore(" crashes") == number)
        crash();

    if (says("crash when made"))
        crash();
    if (says("abort when made"))
        abortHere();
    if (says("throw when made"))
        throw std::runtime_error("the test plugin's own exception as it was made");
    if (says("overflow when made"))
        static_cast<void>(overflow(0));
    if (says("fail fast when made"))
    {
#if defined(_WIN32)
        __fastfail(FAST_FAIL_FATAL_APP_EXIT);
#else
        abortHere();
#endif
    }
    if (says("exit when made"))
        std::exit(0);
    if (says("hang when made"))
        hang();

    // Memory taken and kept, each page written so the system counts it.
    if (const int megabytes = numberAfter("hungry "); megabytes > 0)
    {
        static std::vector<std::unique_ptr<char[]>> kept;
        const auto size = static_cast<std::size_t>(megabytes) * 1024 * 1024;
        kept.push_back(std::make_unique<char[]>(size));
        for (std::size_t i = 0; i < size; i += 4096)
            kept.back()[i] = static_cast<char>(i);
    }

    // Damage that shows only later or elsewhere: a thread that crashes two seconds on, and the C locale set to German.
    // Memory freed twice isn't among it, as the systems' allocators stop that at once.
    if (says("damage"))
    {
        std::thread([] {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            crash();
        }).detach();
        for (const char* german : {"de_DE.UTF-8", "de_DE.utf8", "de_DE", "German_Germany.1252"})
            if (std::setlocale(LC_ALL, german) != nullptr)
                break;
    }
}

class Passing : public AudioEffect
{
public:
    tresult PLUGIN_API initialize(FUnknown* context) override
    {
        const auto result = AudioEffect::initialize(context);
        if (result == kResultOk)
        {
            addAudioInput(STR16("Input"), SpeakerArr::kStereo);
            addAudioOutput(STR16("Output"), SpeakerArr::kStereo);
        }
        return result;
    }

    tresult PLUGIN_API process(ProcessData& data) override
    {
        if (data.numSamples <= 0 || data.numInputs < 1 || data.numOutputs < 1)
            return kResultOk;
        const auto& in = data.inputs[0];
        auto& out = data.outputs[0];
        for (int32 channel = 0; channel < std::min(in.numChannels, out.numChannels); ++channel)
            if (in.channelBuffers32[channel] != out.channelBuffers32[channel])
                std::memcpy(out.channelBuffers32[channel], in.channelBuffers32[channel],
                            sizeof(float) * static_cast<std::size_t>(data.numSamples));
        return kResultOk;
    }
};

// The module's plugins: one, or as many as "many" says, each known by an ID made from the bundle's name and its number.
int classCount()
{
    return std::max(1, numberAfter("many "));
}

std::array<char, 16> classId(int number)
{
    std::uint64_t hash = 14695981039346656037ull;
    for (const char c : name())
        hash = (hash ^ static_cast<unsigned char>(c)) * 1099511628211ull;
    std::array<char, 16> id = {'S', 'y', 'E', 'n'};
    for (std::size_t i = 0; i < 8; ++i)
        id[4 + i] = static_cast<char>(hash >> (8 * i));
    for (std::size_t i = 0; i < 4; ++i)
        id[12 + i] = static_cast<char>(static_cast<std::uint32_t>(number) >> (8 * (3 - i)));
    return id;
}

void copyText(char8* field, std::size_t size, const std::string& text)
{
    std::memset(field, 0, size);
    std::memcpy(field, text.data(), std::min(size - 1, text.size()));
}

// Its one factory, which lives as long as the process: the SDK's interfaces have no virtual destructors.
#if defined(__GNUC__)
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
#endif
class Factory final : public IPluginFactory2
{
public:
    tresult PLUGIN_API queryInterface(const TUID interfaceId, void** object) override
    {
        if (FUnknownPrivate::iidEqual(interfaceId, IPluginFactory2::iid) ||
            FUnknownPrivate::iidEqual(interfaceId, IPluginFactory::iid) ||
            FUnknownPrivate::iidEqual(interfaceId, FUnknown::iid))
        {
            *object = static_cast<IPluginFactory2*>(this);
            return kResultOk;
        }
        *object = nullptr;
        return kNoInterface;
    }

    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }

    tresult PLUGIN_API getFactoryInfo(PFactoryInfo* info) override
    {
        *info = PFactoryInfo("SymbolEngine Tests", "https://symbolengine.dev", "", PFactoryInfo::kUnicode);
        return kResultOk;
    }

    int32 PLUGIN_API countClasses() override { return classCount(); }

    tresult PLUGIN_API getClassInfo(int32 index, PClassInfo* info) override
    {
        if (index < 0 || index >= classCount())
            return kInvalidArgument;
        *info = PClassInfo(classId(index + 1).data(), PClassInfo::kManyInstances, kVstAudioEffectClass,
                           classCount() > 1 ? ("Plugin " + std::to_string(index + 1)).c_str() : name().c_str());
        return kResultOk;
    }

    tresult PLUGIN_API getClassInfo2(int32 index, PClassInfo2* info) override
    {
        if (index < 0 || index >= classCount())
            return kInvalidArgument;
        *info = PClassInfo2(classId(index + 1).data(), PClassInfo::kManyInstances, kVstAudioEffectClass, "", 0, "Fx",
                            "SymbolEngine Tests", "1.0.0", kVstVersionString);
        copyText(info->name, sizeof info->name, classCount() > 1 ? "Plugin " + std::to_string(index + 1) : name());
        return kResultOk;
    }

    tresult PLUGIN_API createInstance(FIDString cid, FIDString interfaceId, void** object) override
    {
        *object = nullptr;
        for (int number = 1; number <= classCount(); ++number)
            if (std::memcmp(cid, classId(number).data(), sizeof(TUID)) == 0)
            {
                onMake(number);
                auto* plugin = new Passing;
                const auto result = plugin->queryInterface(interfaceId, object);
                plugin->release();
                return result;
            }
        return kNoInterface;
    }
};
#if defined(__GNUC__)
    #pragma GCC diagnostic pop
#endif

}

extern "C" SMTG_EXPORT_SYMBOL IPluginFactory* PLUGIN_API GetPluginFactory()
{
    static Factory factory;
    return &factory;
}
