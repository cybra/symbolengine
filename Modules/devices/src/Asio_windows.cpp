#include "AsioDetail.h"
#include "AsioSystem.h"

#ifndef NOMINMAX
    #define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>

// Steinberg's SDK, from the developer's own copy, after the system's headers: it defines platform names of its own.
#include <asiosys.h>
#include <asio.h>
#include <iasiodrv.h>

// COM's interfaces have no virtual destructors, by design.
#if defined(__GNUC__)
    #pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <functional>
#include <deque>
#include <map>
#include <mutex>
#include <new>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

// The SDK's structures are packed to 4 bytes, which a driver built with it expects; checked where it applies.
#if defined(_MSC_VER) && defined(_WIN64)
static_assert(sizeof(ASIOTime) == 148 && offsetof(ASIOTime, timeInfo) == 16);
static_assert(offsetof(AsioTimeInfo, samplePosition) == 16 && offsetof(AsioTimeInfo, flags) == 32);
static_assert(sizeof(ASIOBufferInfo) == 24 && sizeof(ASIOChannelInfo) == 52);
#endif

// ASIO behind the devices module's system layer. Drivers are listed from the registry without loading any, since
// loading drivers to list them has hung programs and cut others' sound, and a driver is loaded when a stream opens
// it: a process holds one at a time, its input and output in one stream. Every call to a driver is made from one
// thread of its own, a single-threaded COM apartment with a message loop and a window, in ASIO's order of states, a
// fault in it caught and its time bounded; structures handed to it are larger than it needs and cleared, and its
// strings bounded. The driver's callbacks carry nothing to say whose they are, so the one driver held reaches its
// stream through the process's current runtime, which a callback in flight is waited out of before it is let go of.
// At each buffer switch that half's input is read, the program called, that half's output written and the driver
// told it is ready; before the start and after the stop the callbacks write silence. A driver asking to reset,
// resync or say its latency changed asks the stream to open it again, which loads it afresh.
namespace symbolengine::asio::detail
{

namespace
{
using namespace std::chrono_literals;

std::int64_t steadyNow() noexcept
{
    static const auto frequency = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return static_cast<std::int64_t>(static_cast<double>(counter.QuadPart) * 1e9 / static_cast<double>(frequency));
}

std::wstring wide(const std::string& text)
{
    const auto size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(std::max(size, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

std::string narrow(const std::wstring& text, UINT codePage = CP_UTF8)
{
    const auto size =
        WideCharToMultiByte(codePage, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(std::max(size, 0)), '\0');
    WideCharToMultiByte(codePage, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
    return out;
}

// A driver's string as UTF-8: as it is when it is UTF-8, otherwise in the system's code page, as older drivers
// write, with spaces at either end taken off.
std::string textOf(const char* given, std::size_t most)
{
    auto text = bounded(given, most);
    if (!isUtf8(text))
    {
        const auto size = MultiByteToWideChar(CP_ACP, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
        std::wstring converted(static_cast<std::size_t>(std::max(size, 0)), L'\0');
        MultiByteToWideChar(CP_ACP, 0, text.data(), static_cast<int>(text.size()), converted.data(), size);
        text = narrow(converted);
    }
    const auto first = text.find_first_not_of(' ');
    if (first == std::string::npos)
        return {};
    return text.substr(first, text.find_last_not_of(' ') - first + 1);
}

SampleLayout layoutOf(ASIOSampleType type) noexcept
{
    switch (type)
    {
        case ASIOSTInt16LSB: return {2, 16, false, false};
        case ASIOSTInt24LSB: return {3, 24, false, false};
        case ASIOSTInt32LSB: return {4, 32, false, false};
        case ASIOSTFloat32LSB: return {4, 32, true, false};
        case ASIOSTFloat64LSB: return {8, 64, true, false};
        case ASIOSTInt32LSB16: return {4, 16, false, false};
        case ASIOSTInt32LSB18: return {4, 18, false, false};
        case ASIOSTInt32LSB20: return {4, 20, false, false};
        case ASIOSTInt32LSB24: return {4, 24, false, false};
        case ASIOSTInt16MSB: return {2, 16, false, true};
        case ASIOSTInt24MSB: return {3, 24, false, true};
        case ASIOSTInt32MSB: return {4, 32, false, true};
        case ASIOSTFloat32MSB: return {4, 32, true, true};
        case ASIOSTFloat64MSB: return {8, 64, true, true};
        case ASIOSTInt32MSB16: return {4, 16, false, true};
        case ASIOSTInt32MSB18: return {4, 18, false, true};
        case ASIOSTInt32MSB20: return {4, 20, false, true};
        case ASIOSTInt32MSB24: return {4, 24, false, true};
        default: return {};
    }
}

constexpr std::array<double, 18> standardRates{8000.0,   11025.0,  16000.0,  22050.0,  24000.0,  32000.0,
                                                44100.0,  48000.0,  64000.0,  88200.0,  96000.0,  128000.0,
                                                176400.0, 192000.0, 352800.0, 384000.0, 705600.0, 768000.0};

// The most channels a driver is believed to have each way; more is a driver giving counts that can't be right.
constexpr long mostChannels = 1024;

// Runs a call into a driver with a fault in it caught, as Windows raises one, so a driver that crashes is let go of
// instead of taking the program with it. What is called holds nothing that needs unwinding.
template <typename Call>
bool guarded(const Call& call) noexcept
{
#if defined(_MSC_VER)
    __try
    {
        call();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
    return true;
#else
    call();
    return true;
#endif
}

// An array a driver writes into, laid at the end of its pages with a page after it that nothing may touch, so a
// driver writing further past it than the room it has faults, which is caught, instead of writing over what lies
// beyond.
template <typename T>
class Guarded
{
    static_assert(std::is_trivially_destructible_v<T>);

public:
    Guarded() = default;
    ~Guarded() { release(); }
    Guarded(const Guarded&) = delete;
    Guarded& operator=(const Guarded&) = delete;

    // Room for count values, each cleared; false when there is no memory for it.
    bool assign(std::size_t count)
    {
        release();
        SYSTEM_INFO system{};
        GetSystemInfo(&system);
        const std::size_t page = system.dwPageSize, align = alignof(std::max_align_t);
        const auto bytes = (std::max<std::size_t>(count, 1) * sizeof(T) + align - 1) / align * align;
        const auto pages = (bytes + page - 1) / page + 1;
        base = VirtualAlloc(nullptr, pages * page, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (base == nullptr)
            return false;
        DWORD before = 0;
        auto* guard = static_cast<char*>(base) + (pages - 1) * page;
        VirtualProtect(guard, page, PAGE_NOACCESS, &before);
        values = reinterpret_cast<T*>(guard - bytes);
        for (std::size_t i = 0; i < count; ++i)
            new (values + i) T{};
        size = count;
        return true;
    }

    T* data() const noexcept { return values; }
    T& operator[](std::size_t i) const noexcept { return values[i]; }
    T* begin() const noexcept { return values; }
    T* end() const noexcept { return values + size; }

private:
    void release() noexcept
    {
        if (base != nullptr)
            VirtualFree(base, 0, MEM_RELEASE);
        base = nullptr;
        values = nullptr;
        size = 0;
    }

    void* base = nullptr;
    T* values = nullptr;
    std::size_t size = 0;
};

HINSTANCE thisModule() noexcept
{
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&thisModule), &module);
    return module;
}

const wchar_t* windowClass() noexcept
{
    static const auto* name = [] {
        WNDCLASSW type{};
        type.lpfnWndProc = DefWindowProcW;
        type.hInstance = thisModule();
        type.lpszClassName = L"SymbolEngineAsioDriver";
        RegisterClassW(&type);
        return L"SymbolEngineAsioDriver";
    }();
    return name;
}

constexpr UINT runJobs = WM_APP + 1, endLoop = WM_APP + 2;

// The thread every call to one driver is made from: a single-threaded COM apartment with a message loop and a window
// of its own, which the driver is given. Its state is the loop's as much as the owner's, so either can end first.
class DriverThread
{
public:
    DriverThread()
    {
        thread = std::thread([state = state] { loop(state); });
        std::unique_lock hold(state->lock);
        state->changed.wait(hold, [this] { return state->started; });
    }

    ~DriverThread()
    {
        if (state->window != nullptr)
            PostMessageW(state->window, endLoop, 0, 0);
        // A thread held up in a driver's call, or this destructor run on the thread itself, is left to end alone.
        if (state->stuck.load() || std::this_thread::get_id() == thread.get_id())
            thread.detach();
        else
            thread.join();
    }

    DriverThread(const DriverThread&) = delete;
    DriverThread& operator=(const DriverThread&) = delete;

    HWND window() const noexcept { return state->window; }
    bool stuck() const noexcept { return state->stuck.load(); }

    // Runs work on the thread without waiting for it, as a driver's control panel, which may return only when the user
    // closes it; false when the thread has no window or is stuck.
    bool post(std::function<void()> work)
    {
        if (state->window == nullptr || state->stuck.load())
            return false;
        auto job = std::make_shared<Job>();
        job->work = std::move(work);
        {
            const std::lock_guard hold(state->lock);
            state->jobs.push_back(job);
        }
        PostMessageW(state->window, runJobs, 0, 0);
        return true;
    }

    // Runs work on the thread, waiting at most bound; false when the thread has no window or didn't finish in time,
    // after which it is taken as stuck and nothing more is run on it.
    bool run(std::function<void()> work, std::chrono::milliseconds bound)
    {
        if (state->window == nullptr || state->stuck.load())
            return false;
        auto job = std::make_shared<Job>();
        job->work = std::move(work);
        {
            const std::lock_guard hold(state->lock);
            state->jobs.push_back(job);
        }
        PostMessageW(state->window, runJobs, 0, 0);
        std::unique_lock hold(state->lock);
        if (!state->changed.wait_for(hold, bound, [&] { return job->done; }))
        {
            state->stuck = true;
            return false;
        }
        return true;
    }

private:
    struct Job
    {
        std::function<void()> work;
        bool done = false;
    };

    struct State
    {
        std::mutex lock;
        std::condition_variable changed;
        std::deque<std::shared_ptr<Job>> jobs;
        HWND window = nullptr;
        bool started = false;
        std::atomic<bool> stuck{false};
    };

    static void loop(std::shared_ptr<State> state)
    {
        const auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        auto* window = CreateWindowExW(0, windowClass(), L"", WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr,
                                       thisModule(), nullptr);
        {
            const std::lock_guard hold(state->lock);
            state->window = window;
            state->started = true;
        }
        state->changed.notify_all();

        MSG message{};
        while (window != nullptr && GetMessageW(&message, nullptr, 0, 0) > 0)
        {
            if (message.hwnd == window && message.message == endLoop)
                break;
            if (message.hwnd == window && message.message == runJobs)
            {
                drain(*state);
                continue;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (window != nullptr)
            DestroyWindow(window);
        if (SUCCEEDED(com))
            CoUninitialize();
    }

    static void drain(State& state)
    {
        for (;;)
        {
            std::shared_ptr<Job> job;
            {
                const std::lock_guard hold(state.lock);
                if (state.jobs.empty())
                    return;
                job = state.jobs.front();
                state.jobs.pop_front();
            }
            job->work();
            // What the work held is let go of before its caller hears it is done, so the caller lets go last.
            job->work = nullptr;
            {
                const std::lock_guard hold(state.lock);
                job->done = true;
            }
            state.changed.notify_all();
        }
    }

    std::shared_ptr<State> state = std::make_shared<State>();
    std::thread thread;
};

// What the driver's callbacks reach while its buffers are made: the stream's client, the buffers and their types.
struct Runtime
{
    std::atomic<AudioSystemClient*> client{nullptr};
    std::atomic<bool> running{false}, inSwitch{false}, gap{false}, reopenAsked{false}, resetAsked{false};

    // While the driver's control panel is open, a reset it asks for waits for the panel to close, and a change of rate
    // is read once it has.
    std::atomic<bool> panelOpen{false}, reopenAfterPanel{false};
    IASIO* driver = nullptr;
    bool tellOutputReady = false;
    int frames = 0;
    double rate = 0.0;
    std::atomic<std::int64_t> inputLatency{-1}, outputLatency{-1};
    std::vector<SampleLayout> inputLayouts, outputLayouts;
    std::vector<std::array<void*, 2>> inputBuffers, outputBuffers;
    std::vector<std::vector<float>> inputs, outputs;
    std::vector<const float*> inputPointers;
    std::vector<float*> outputPointers;

    void silence(std::size_t half) noexcept
    {
        for (std::size_t c = 0; c < outputBuffers.size(); ++c)
            if (outputBuffers[c][half] != nullptr)
                std::memset(outputBuffers[c][half], 0,
                            static_cast<std::size_t>(frames) * static_cast<std::size_t>(outputLayouts[c].bytes));
    }

    void ready() noexcept
    {
        if (tellOutputReady && driver != nullptr)
            driver->outputReady();
    }

    std::int64_t nanoseconds(const std::atomic<std::int64_t>& given) const noexcept
    {
        const auto latency = given.load();
        return latency < 0 || rate <= 0.0 ? 0 : static_cast<std::int64_t>(static_cast<double>(latency) * 1e9 / rate);
    }

    void cycle(long index, const ASIOTime* time) noexcept
    {
        if (index != 0 && index != 1)
            return;
        const auto half = static_cast<std::size_t>(index);
        const auto now = steadyNow();

        // Called again before the last call returned: this half goes out silent, and what it lacks is a gap.
        if (inSwitch.exchange(true))
        {
            silence(half);
            gap = true;
            return;
        }

        auto* to = running.load() ? client.load() : nullptr;
        if (to == nullptr)
        {
            silence(half);
            ready();
            inSwitch = false;
            return;
        }

        const auto count = static_cast<std::size_t>(frames);
        for (std::size_t c = 0; c < inputs.size(); ++c)
            read(inputLayouts[c], inputBuffers[c][half], inputs[c].data(), count);

        AudioSystemCycle given;
        given.inputs = inputPointers.data();
        given.outputs = outputPointers.data();
        given.inputChannels = static_cast<int>(inputPointers.size());
        given.outputChannels = static_cast<int>(outputPointers.size());
        given.frames = count;
        if (time != nullptr && (time->timeInfo.flags & kSamplePositionValid) != 0)
        {
            const auto& at = time->timeInfo.samplePosition;
            const auto position =
                static_cast<std::int64_t>((static_cast<std::uint64_t>(at.hi) << 32) | static_cast<std::uint64_t>(at.lo));
            given.inputPosition = position;
            given.outputPosition = position;
        }
        given.inputGap = given.outputGap = gap.exchange(false);
        given.inputTime = now - nanoseconds(inputLatency);
        given.outputTime = now + nanoseconds(outputLatency);
        to->cycle(given);

        for (std::size_t c = 0; c < outputs.size(); ++c)
            write(outputLayouts[c], outputs[c].data(), outputBuffers[c][half], count);
        ready();
        inSwitch = false;
    }

    // A notice of the rate the driver already runs at is passed over, and a rate of 0 is its clock lost.
    void rateChanged(double given) noexcept
    {
        auto* to = client.load();
        if (to == nullptr)
            return;
        if (given <= 0.0)
            to->lost(AudioLoss::clockLost, ASE_NoClock);
        else if (std::abs(given - rate) >= 0.5 && !panelOpen.load())
            to->changed();
    }

    void askToReopen() noexcept
    {
        resetAsked = true;
        if (panelOpen.load())
        {
            reopenAfterPanel = true;
            return;
        }
        if (auto* to = client.load())
            to->reopen();
        else
            reopenAsked = true;
    }
};

std::atomic<Runtime*> current{nullptr};
std::atomic<int> callbacksIn{0};

// ASIO lets a process hold one driver at a time.
std::atomic<bool> holding{false};

// A callback under way, counted so the runtime it reaches is kept until it returns.
struct Inside
{
    Inside() noexcept
    {
        callbacksIn.fetch_add(1);
        runtime = current.load();
    }
    ~Inside() { callbacksIn.fetch_sub(1); }
    Inside(const Inside&) = delete;
    Inside& operator=(const Inside&) = delete;
    Runtime* runtime = nullptr;
};

void bufferSwitch(long index, ASIOBool)
{
    const Inside inside;
    if (inside.runtime != nullptr)
        inside.runtime->cycle(index, nullptr);
}

ASIOTime* bufferSwitchTimeInfo(ASIOTime* time, long index, ASIOBool)
{
    const Inside inside;
    if (inside.runtime != nullptr)
        inside.runtime->cycle(index, time);
    return nullptr;
}

void sampleRateDidChange(ASIOSampleRate rate)
{
    const Inside inside;
    if (inside.runtime != nullptr)
        inside.runtime->rateChanged(rate);
}

long asioMessage(long selector, long value, void*, double*)
{
    const Inside inside;
    auto* runtime = inside.runtime;
    switch (selector)
    {
        case kAsioSelectorSupported:
            return value == kAsioEngineVersion || value == kAsioResetRequest || value == kAsioBufferSizeChange ||
                           value == kAsioResyncRequest || value == kAsioLatenciesChanged ||
                           value == kAsioSupportsTimeInfo || value == kAsioSupportsTimeCode || value == kAsioOverload
                       ? 1
                       : 0;
        case kAsioEngineVersion: return 2;
        case kAsioResetRequest:
        case kAsioBufferSizeChange:
        case kAsioResyncRequest:
        case kAsioLatenciesChanged:
            if (runtime != nullptr)
            {
                if (selector == kAsioResyncRequest)
                    runtime->gap = true;
                runtime->askToReopen();
            }
            return 1;
        case kAsioSupportsTimeInfo: return 1;
        case kAsioSupportsTimeCode: return 0;
        case kAsioOverload:
            if (runtime != nullptr)
                runtime->gap = true;
            return 1;
        default: return 0;
    }
}

ASIOCallbacks callbacks{&bufferSwitch, &sampleRateDidChange, &asioMessage, &bufferSwitchTimeInfo};

// One driver loaded, with its thread, from its load to its release. Calls to it write into its own scratch, so a
// call that never returns writes nowhere that is gone.
class Loaded : public std::enable_shared_from_this<Loaded>
{
public:
    Loaded(Host given, std::string driverId, std::string driverName)
        : host(std::move(given)), id(std::move(driverId)), name(std::move(driverName))
    {
        scratch.text.assign(4096);
    }

    Runtime runtime;

    AudioError load()
    {
        if (thread.window() == nullptr)
            return {"The ASIO driver's thread didn't start.", "load"};
        auto self = shared_from_this();
        const bool finished = thread.run(
            [self] {
                IASIO* made = nullptr;
                std::string why;
                if (!guarded([&] { made = self->host.make ? self->host.make(self->id, why) : nullptr; }))
                {
                    self->faulted = true;
                    why = "The driver crashed as it loaded.";
                }
                self->driver = made;
                self->problem = why;
            },
            host.callTimeout);
        if (!finished)
            return {"The ASIO driver didn't load in time.", "load"};
        if (driver == nullptr)
            return {problem.empty() ? "The ASIO driver didn't load." : problem, "load"};

        HWND window = thread.window();
        if (!call([window](IASIO& d, Scratch& s) { s.result = d.init(window); }))
            return lost("init");
        if (scratch.result <= 0)
        {
            call([](IASIO& d, Scratch& s) { d.getErrorMessage(s.text.data()); });
            const auto message = scratch.text.data() != nullptr ? textOf(scratch.text.data(), 124) : std::string();
            return {message.empty() ? "The ASIO driver didn't start." : "The ASIO driver didn't start: " + message + ".",
                    "init", scratch.result};
        }
        return {};
    }

    // What the driver has and runs at, as it says now. Its channel counts are read first, since they can change with
    // its rate.
    AudioError read(AudioDeviceInfo& into)
    {
        if (!call([](IASIO& d, Scratch& s) { s.result = d.getChannels(&s.values[0], &s.values[8]); }))
            return lost("getChannels");
        if (scratch.result != ASE_OK)
            return {scratch.result == ASE_NotPresent ? "The ASIO driver's interface isn't connected."
                                                      : "The ASIO driver didn't say its channels.",
                    "getChannels", scratch.result};
        const auto inputCount = scratch.values[0], outputCount = scratch.values[8];
        if (inputCount < 0 || outputCount < 0 || inputCount > mostChannels || outputCount > mostChannels ||
            inputCount + outputCount == 0)
            return {"The ASIO driver gave channel counts that can't be right.", "getChannels"};

        if (!scratch.channels.assign(static_cast<std::size_t>(inputCount + outputCount)))
            return {"There isn't the memory to read the ASIO driver's channels.", "getChannelInfo"};
        for (long i = 0; i < inputCount + outputCount; ++i)
        {
            auto& channel = scratch.channels[static_cast<std::size_t>(i)].info;
            channel.isInput = i < inputCount ? ASIOTrue : ASIOFalse;
            channel.channel = i < inputCount ? i : i - inputCount;
        }
        if (!call([](IASIO& d, Scratch& s) {
                for (auto& channel : s.channels)
                    if (d.getChannelInfo(&channel.info) != ASE_OK)
                        channel.info.type = -1;
            }))
            return lost("getChannelInfo");

        AudioDeviceInfo info;
        info.id = id;
        info.name = name;
        inputTypes.clear();
        outputTypes.clear();
        for (long i = 0; i < inputCount + outputCount; ++i)
        {
            const auto& channel = scratch.channels[static_cast<std::size_t>(i)].info;
            const bool input = i < inputCount;
            const auto number = (input ? i : i - inputCount) + 1;
            auto label = textOf(channel.name, sizeof(channel.name));
            if (label.empty())
                label = (input ? "Input " : "Output ") + std::to_string(number);
            (input ? info.inputNames : info.outputNames).push_back(label);
            (input ? inputTypes : outputTypes).push_back(layoutOf(channel.type));
        }

        if (!call([](IASIO& d, Scratch& s) {
                s.result = d.getBufferSize(&s.values[0], &s.values[1], &s.values[2], &s.values[3]);
                s.second = d.getSampleRate(&s.rates[0]);
                for (std::size_t r = 0; r < standardRates.size(); ++r)
                    s.offered[r] = d.canSampleRate(standardRates[r]) == ASE_OK;
            }))
            return lost("getBufferSize");
        if (scratch.result != ASE_OK)
            return {"The ASIO driver didn't say its buffer sizes.", "getBufferSize", scratch.result};
        info.blockSizes = blockSizes(scratch.values[0], scratch.values[1], scratch.values[2], scratch.values[3]);
        if (info.blockSizes.empty())
            return {"The ASIO driver gave buffer sizes that can't be right.", "getBufferSize"};
        preferred = info.blockSizes.front();
        info.currentBlock = runtime.frames > 0 ? runtime.frames : preferred;

        info.currentRate = scratch.second == ASE_OK && scratch.rates[0] > 0.0 ? scratch.rates[0] : 0.0;
        for (std::size_t r = 0; r < standardRates.size(); ++r)
            if (scratch.offered[r])
                info.sampleRates.push_back(standardRates[r]);
        if (info.currentRate > 0.0 &&
            std::none_of(info.sampleRates.begin(), info.sampleRates.end(),
                         [&](double offered) { return std::abs(offered - info.currentRate) < 0.5; }))
        {
            info.sampleRates.push_back(info.currentRate);
            std::sort(info.sampleRates.begin(), info.sampleRates.end());
        }
        rate = info.currentRate;
        into = std::move(info);
        return {};
    }

    // Sets a rate and reads it back after the driver has had time to settle, since drivers say yes and stay put.
    AudioError setRate(double wanted)
    {
        if (!call([wanted](IASIO& d, Scratch& s) { s.result = d.setSampleRate(wanted); }))
            return lost("setSampleRate");
        if (scratch.result != ASE_OK)
            return {"The ASIO driver didn't change to that rate.", "setSampleRate", scratch.result};
        std::this_thread::sleep_for(10ms);
        if (!call([](IASIO& d, Scratch& s) { s.result = d.getSampleRate(&s.rates[0]); }))
            return lost("getSampleRate");
        if (scratch.result != ASE_OK || std::abs(scratch.rates[0] - wanted) >= 0.5)
            return {"The ASIO driver said it changed to that rate, but runs at another.", "setSampleRate"};
        rate = scratch.rates[0];
        return {};
    }

    int preferredSize() const noexcept { return preferred; }

    // Makes the buffers for the channels asked for, at the size asked or else the driver's preferred one.
    AudioError prepare(const AudioStreamRequest& request, int size)
    {
        for (const auto channel : request.inputChannels)
            if (channel < 0 || channel >= static_cast<int>(inputTypes.size()))
                return {"The ASIO driver doesn't have that input channel.", "open"};
        for (const auto channel : request.outputChannels)
            if (channel < 0 || channel >= static_cast<int>(outputTypes.size()))
                return {"The ASIO driver doesn't have that output channel.", "open"};
        for (const auto channel : request.inputChannels)
            if (inputTypes[static_cast<std::size_t>(channel)].bytes == 0)
                return {"The ASIO driver gives that input channel in a form that isn't converted.", "open"};
        for (const auto channel : request.outputChannels)
            if (outputTypes[static_cast<std::size_t>(channel)].bytes == 0)
                return {"The ASIO driver takes that output channel in a form that isn't converted.", "open"};

        const auto inputCount = request.inputChannels.size(), outputCount = request.outputChannels.size();
        const auto count = inputCount + outputCount;
        if (count == 0)
            return {"No channels were asked for.", "open"};

        // The driver's callbacks may come while it makes its buffers, so the runtime is in place before.
        runtime.rate = rate;
        runtime.driver = driver;
        runtime.inputLayouts.clear();
        runtime.outputLayouts.clear();
        for (const auto channel : request.inputChannels)
            runtime.inputLayouts.push_back(inputTypes[static_cast<std::size_t>(channel)]);
        for (const auto channel : request.outputChannels)
            runtime.outputLayouts.push_back(outputTypes[static_cast<std::size_t>(channel)]);
        current.store(&runtime);

        auto made = make(request, size);
        if (made && size != preferred && !faulted.load())
            made = make(request, preferred);
        if (made)
        {
            letGoOfRuntime();
            if (faulted.load())
                return lost("createBuffers");
            return {"The ASIO driver didn't make its buffers.", "createBuffers", made};
        }
        prepared = true;

        runtime.inputBuffers.clear();
        runtime.outputBuffers.clear();
        for (std::size_t i = 0; i < count; ++i)
        {
            const auto& buffer = scratch.buffers[i];
            if (buffer.buffers[0] == nullptr || buffer.buffers[1] == nullptr)
                return {"The ASIO driver gave buffers that aren't there.", "createBuffers"};
            (i < inputCount ? runtime.inputBuffers : runtime.outputBuffers).push_back({buffer.buffers[0], buffer.buffers[1]});
        }

        const auto frames = static_cast<std::size_t>(runtime.frames);
        runtime.inputs.assign(inputCount, std::vector<float>(frames, 0.0f));
        runtime.outputs.assign(outputCount, std::vector<float>(frames, 0.0f));
        runtime.inputPointers.clear();
        runtime.outputPointers.clear();
        for (auto& channel : runtime.inputs)
            runtime.inputPointers.push_back(channel.data());
        for (auto& channel : runtime.outputs)
            runtime.outputPointers.push_back(channel.data());

        readLatencies();
        if (call([](IASIO& d, Scratch& s) { s.result = d.outputReady(); }))
            runtime.tellOutputReady = scratch.result == ASE_OK;
        if (faulted.load())
            return lost("getLatencies");
        runtime.silence(0);
        runtime.silence(1);
        return {};
    }

    void readLatencies()
    {
        if (runtime.panelOpen.load())
            return;
        if (call([](IASIO& d, Scratch& s) { s.result = d.getLatencies(&s.values[0], &s.values[8]); }) &&
            scratch.result == ASE_OK && scratch.values[0] >= 0 && scratch.values[8] >= 0)
        {
            runtime.inputLatency = runtime.inputBuffers.empty() ? -1 : scratch.values[0];
            runtime.outputLatency = runtime.outputBuffers.empty() ? -1 : scratch.values[8];
        }
        else
        {
            runtime.inputLatency = -1;
            runtime.outputLatency = -1;
        }
    }

    AudioError start(AudioSystemClient& to)
    {
        runtime.silence(0);
        runtime.silence(1);
        runtime.client.store(&to);
        runtime.running = true;
        if (!call([](IASIO& d, Scratch& s) { s.result = d.start(); }) || scratch.result != ASE_OK)
        {
            runtime.running = false;
            runtime.client.store(nullptr);
            if (faulted.load())
                return lost("start");
            return {"The ASIO driver didn't start.", "start", scratch.result};
        }
        started = true;
        if (runtime.reopenAsked.exchange(false))
            to.reopen();
        return {};
    }

    void stop() noexcept
    {
        if (!started)
            return;
        started = false;
        runtime.running = false;
        // A reset the driver asks for as it stops is for a stream that is going.
        runtime.client.store(nullptr);
        call([](IASIO& d, Scratch&) { d.stop(); });
        runtime.silence(0);
        runtime.silence(1);
    }

    // Back through ASIO's states: stopped, its buffers let go of, then the driver. The callbacks stop reaching the
    // runtime first, so one the driver makes after this touches no buffer of its own it has let go of. Letting go of
    // the driver is always tried, even after a fault; one that faults then, or a thread held up in it, is left be.
    void close() noexcept
    {
        stop();
        letGoOfRuntime();
        if (prepared)
        {
            call([](IASIO& d, Scratch&) { d.disposeBuffers(); });
            prepared = false;
        }
        if (driver != nullptr && !thread.stuck())
        {
            auto self = shared_from_this();
            thread.run(
                [self] {
                    IASIO* d = self->driver;
                    if (guarded([d] { d->Release(); }))
                        self->driver = nullptr;
                },
                host.callTimeout);
        }
    }

    // The driver's control panel, opened from its thread without waiting, as some return only when the user closes
    // them. Once it has closed, a reset asked for while it was open opens the stream again, at what was chosen there;
    // otherwise the stream reads the driver again, in case anything changed.
    AudioError showSettings()
    {
        if (faulted.load() || driver == nullptr || thread.stuck())
            return lost("controlPanel");
        if (runtime.panelOpen.exchange(true))
            return {};
        auto self = shared_from_this();
        const bool posted = thread.post([self] {
            IASIO* d = self->driver;
            if (!guarded([d] { d->controlPanel(); }))
                self->faulted = true;
            self->runtime.panelOpen = false;
            const bool reopen = self->runtime.reopenAfterPanel.exchange(false);
            if (auto* to = self->runtime.client.load())
            {
                if (reopen)
                    to->reopen();
                else
                    to->changed();
            }
        });
        if (!posted)
        {
            runtime.panelOpen = false;
            return lost("controlPanel");
        }
        return {};
    }

    // The rate the driver runs at now, and the size it would take now, for a stream asked to open it again.
    double rateNow()
    {
        if (runtime.panelOpen.load())
            return rate;
        if (call([](IASIO& d, Scratch& s) { s.result = d.getSampleRate(&s.rates[0]); }) && scratch.result == ASE_OK)
            return scratch.rates[0];
        return rate;
    }

    int sizeNow()
    {
        if (!runtime.resetAsked.load() || runtime.panelOpen.load())
            return runtime.frames;
        if (call([](IASIO& d, Scratch& s) {
                s.result = d.getBufferSize(&s.values[0], &s.values[1], &s.values[2], &s.values[3]);
            }) &&
            scratch.result == ASE_OK)
        {
            const auto sizes = blockSizes(scratch.values[0], scratch.values[1], scratch.values[2], scratch.values[3]);
            if (!sizes.empty())
                return sizes.front();
        }
        return runtime.frames;
    }

private:
    struct PaddedChannel
    {
        ASIOChannelInfo info{};
        char spare[1024]{};
    };

    // Where calls to the driver write, larger than any of them needs, as drivers write past what they are given.
    struct Scratch
    {
        ASIOError result = 0, second = 0;
        long values[16]{};
        double rates[4]{};
        std::array<bool, standardRates.size()> offered{};
        Guarded<char> text;
        Guarded<PaddedChannel> channels;
        Guarded<ASIOBufferInfo> buffers;
    };

    // Runs a call to the driver on its thread, a fault in it caught and its time bounded; false when it faulted or
    // didn't return, after which the driver is called no more but to be let go of.
    template <typename Work>
    bool call(Work work)
    {
        if (faulted.load() || driver == nullptr || thread.stuck())
            return false;
        auto self = shared_from_this();
        const bool finished = thread.run(
            [self, work] {
                IASIO* d = self->driver;
                Scratch* s = &self->scratch;
                if (!guarded([&] { work(*d, *s); }))
                    self->faulted = true;
            },
            host.callTimeout);
        if (!finished)
            faulted = true;
        return finished && !faulted.load();
    }

    AudioError lost(const char* step) const
    {
        return {thread.stuck() ? "The ASIO driver stopped answering." : "The ASIO driver crashed.", step};
    }

    // Makes the buffers at a size, letting go of any made before; the driver's error, or ASE_OK.
    ASIOError make(const AudioStreamRequest& request, int size)
    {
        if (prepared)
        {
            call([](IASIO& d, Scratch&) { d.disposeBuffers(); });
            prepared = false;
        }
        const auto count = request.inputChannels.size() + request.outputChannels.size();
        if (!scratch.buffers.assign(count + 64))
            return ASE_NoMemory;
        std::size_t i = 0;
        for (const auto channel : request.inputChannels)
            scratch.buffers[i++] = {ASIOTrue, channel, {nullptr, nullptr}};
        for (const auto channel : request.outputChannels)
            scratch.buffers[i++] = {ASIOFalse, channel, {nullptr, nullptr}};
        runtime.frames = size;
        const auto channels = static_cast<long>(count);
        if (!call([channels, size](IASIO& d, Scratch& s) {
                s.result = d.createBuffers(s.buffers.data(), channels, size, &callbacks);
            }))
            return ASE_HWMalfunction;
        if (scratch.result == ASE_OK)
            prepared = true;
        return scratch.result;
    }

    // The callbacks stop reaching the runtime, and one under way is waited out, a bounded time.
    void letGoOfRuntime() noexcept
    {
        auto* mine = &runtime;
        current.compare_exchange_strong(mine, nullptr);
        for (int i = 0; i < 1000 && callbacksIn.load() > 0; ++i)
            std::this_thread::sleep_for(1ms);
    }

    Host host;
    std::string id, name, problem;
    DriverThread thread;
    IASIO* driver = nullptr;
    std::atomic<bool> faulted{false};
    bool prepared = false, started = false;
    std::vector<SampleLayout> inputTypes, outputTypes;
    int preferred = 0;
    double rate = 0.0;
    Scratch scratch;
};

class AsioStream final : public AudioSystemStream
{
public:
    explicit AsioStream(std::shared_ptr<Loaded> l) : loaded(std::move(l)) {}

    ~AsioStream() override
    {
        loaded->close();
        holding = false;
    }

    AsioStream(const AsioStream&) = delete;
    AsioStream& operator=(const AsioStream&) = delete;

    AudioStreamInfo info() const override
    {
        AudioStreamInfo now;
        now.sampleRate = loaded->rateNow();
        now.maxBlock = loaded->sizeNow();
        now.inputChannels = static_cast<int>(loaded->runtime.inputBuffers.size());
        now.outputChannels = static_cast<int>(loaded->runtime.outputBuffers.size());
        loaded->readLatencies();
        now.inputLatency = loaded->runtime.inputLatency;
        now.outputLatency = loaded->runtime.outputLatency;
        now.priority = "the ASIO driver's own thread";
        now.unprocessed = now.inputChannels > 0;
        return now;
    }

    bool start(AudioSystemClient& client, AudioError& error) override
    {
        error = loaded->start(client);
        return !error;
    }

    void stop() noexcept override { loaded->stop(); }

    AudioError showSettings() override { return loaded->showSettings(); }

private:
    std::shared_ptr<Loaded> loaded;
};

class AsioSystem final : public AudioSystem
{
public:
    explicit AsioSystem(Host given) : host(std::move(given)) {}

    std::string name() const override { return "ASIO"; }

    std::int64_t now() const noexcept override { return steadyNow(); }

    std::vector<std::string> deviceIds() override
    {
        std::vector<std::string> ids;
        for (const auto& driver : installed())
            ids.push_back(driver.id);
        return ids;
    }

    AudioDeviceInfo describe(const std::string& id) override
    {
        {
            const std::lock_guard hold(lock);
            if (const auto found = read.find(id); found != read.end())
                return found->second;
        }
        AudioDeviceInfo info;
        info.id = id;
        info.unread = true;
        for (const auto& driver : installed())
            if (driver.id == id)
            {
                info.name = driver.name;
                info.answering = driver.problem.empty();
                info.problem = driver.problem;
                return info;
            }
        info.answering = false;
        info.problem = "The ASIO driver isn't installed.";
        return info;
    }

    std::string defaultDevice(AudioDirection) override { return {}; }

    std::unique_ptr<AudioSystemStream> open(const std::string& id, const AudioStreamRequest& request,
                                            AudioError& error) override
    {
        if (holding.exchange(true))
        {
            error = {"ASIO lets a program hold one driver at a time, and another is open.", "load"};
            return nullptr;
        }

        auto listed = describe(id);
        if (!listed.answering)
        {
            holding = false;
            error = {listed.problem, "load"};
            return nullptr;
        }
        auto loaded = std::make_shared<Loaded>(host, id, listed.name.empty() ? id : listed.name);
        const auto fail = [&](AudioError why) -> std::unique_ptr<AudioSystemStream> {
            loaded->close();
            holding = false;
            error = std::move(why);
            return nullptr;
        };

        if (auto why = loaded->load())
            return fail(why);
        AudioDeviceInfo info;
        if (auto why = loaded->read(info))
            return fail(why);
        remember(info);

        // A rate other than the driver's, set and read back, and what the driver has read again at it.
        if (request.sampleRate > 0.0 && std::abs(request.sampleRate - info.currentRate) >= 0.5)
        {
            if (auto why = loaded->setRate(request.sampleRate))
                return fail(why);
            if (auto why = loaded->read(info))
                return fail(why);
            remember(info);
        }

        const auto size = request.blockSize > 0 ? request.blockSize : loaded->preferredSize();
        if (auto why = loaded->prepare(request, size))
            return fail(why);
        return std::make_unique<AsioStream>(std::move(loaded));
    }

    void addListener(AudioSystemListener& listener) override
    {
        const std::lock_guard hold(listening);
        listeners.push_back(&listener);
    }

    void removeListener(AudioSystemListener& listener) override
    {
        const std::lock_guard hold(listening);
        std::erase(listeners, &listener);
    }

private:
    std::vector<Installed> installed() const { return host.drivers ? host.drivers() : std::vector<Installed>{}; }

    // What a driver said once loaded is kept for the lists, which are told to read it.
    void remember(const AudioDeviceInfo& info)
    {
        {
            const std::lock_guard hold(lock);
            read[info.id] = info;
        }
        const std::lock_guard hold(listening);
        for (auto* listener : listeners)
            listener->systemChanged(AudioSystemChange::devices);
    }

    Host host;
    std::mutex lock, listening;
    std::map<std::string, AudioDeviceInfo> read;
    std::vector<AudioSystemListener*> listeners;
};

// The registry's string value, or empty.
std::wstring registryText(HKEY root, const std::wstring& key, const wchar_t* value)
{
    DWORD size = 0;
    if (RegGetValueW(root, key.c_str(), value, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, nullptr, &size) !=
            ERROR_SUCCESS ||
        size == 0)
        return {};
    std::wstring text(size / sizeof(wchar_t) + 1, L'\0');
    if (RegGetValueW(root, key.c_str(), value, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, text.data(), &size) !=
        ERROR_SUCCESS)
        return {};
    text.resize(std::wcslen(text.c_str()));
    return text;
}

// The processor a DLL is built for, from its header, or 0 when it can't be read.
WORD machineOf(const std::wstring& path)
{
    auto* file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return 0;
    WORD machine = 0;
    IMAGE_DOS_HEADER dos{};
    DWORD got = 0;
    if (ReadFile(file, &dos, sizeof(dos), &got, nullptr) && got == sizeof(dos) && dos.e_magic == IMAGE_DOS_SIGNATURE)
    {
        LARGE_INTEGER at{};
        at.QuadPart = dos.e_lfanew;
        DWORD signature = 0;
        IMAGE_FILE_HEADER header{};
        if (SetFilePointerEx(file, at, nullptr, FILE_BEGIN) && ReadFile(file, &signature, sizeof(signature), &got, nullptr) &&
            got == sizeof(signature) && signature == IMAGE_NT_SIGNATURE &&
            ReadFile(file, &header, sizeof(header), &got, nullptr) && got == sizeof(header))
            machine = header.Machine;
    }
    CloseHandle(file);
    return machine;
}

#if defined(_M_ARM64) || defined(__aarch64__)
constexpr WORD ownMachine = IMAGE_FILE_MACHINE_ARM64;
#elif defined(_M_X64) || defined(__x86_64__)
constexpr WORD ownMachine = IMAGE_FILE_MACHINE_AMD64;
#else
constexpr WORD ownMachine = IMAGE_FILE_MACHINE_I386;
#endif

// Why a driver can't be loaded by this program, from what is registered for it, or nothing.
std::string problemWith(const std::wstring& classId)
{
    const auto path = registryText(HKEY_CLASSES_ROOT, L"CLSID\\" + classId + L"\\InprocServer32", nullptr);
    if (path.empty())
        return "The driver isn't installed properly: nothing is registered to load it.";
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
    {
        const auto why = GetLastError();
        if ((why == ERROR_FILE_NOT_FOUND || why == ERROR_PATH_NOT_FOUND) && path.find(L'\\') != std::wstring::npos)
            return "The driver's file is gone.";
        return {};
    }
    if (const auto machine = machineOf(path); machine != 0 && machine != ownMachine)
        return "The driver is built for another kind of processor than this program's, so it can't be loaded here.";
    return {};
}

// Drivers that wrap the system's own older drivers and are known to misbehave, never listed.
bool denied(const std::string& name)
{
    return name.starts_with("ASIO DirectX Full Duplex") || name.starts_with("ASIO Multimedia Driver");
}

std::vector<Installed> registryDrivers()
{
    std::vector<Installed> found;
    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0, KEY_READ, &root) != ERROR_SUCCESS)
        return found;
    for (DWORD i = 0;; ++i)
    {
        wchar_t key[256]{};
        DWORD length = 256;
        if (RegEnumKeyExW(root, i, key, &length, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
            break;
        const auto classId = registryText(root, key, L"CLSID");
        if (classId.empty())
            continue;
        const auto description = registryText(root, key, L"Description");
        Installed driver;
        driver.id = narrow(classId);
        std::transform(driver.id.begin(), driver.id.end(), driver.id.begin(),
                       [](char c) { return static_cast<char>(std::toupper(static_cast<unsigned char>(c))); });
        driver.name = narrow(description.empty() ? std::wstring(key) : description);
        if (denied(driver.name) || std::any_of(found.begin(), found.end(), [&](const Installed& other) {
                return other.id == driver.id;
            }))
            continue;
        driver.problem = problemWith(classId);
        found.push_back(std::move(driver));
    }
    RegCloseKey(root);
    return found;
}

IASIO* comDriver(const std::string& id, std::string& problem)
{
    CLSID classId{};
    if (FAILED(CLSIDFromString(wide(id).c_str(), &classId)))
    {
        problem = "The driver's class ID can't be read.";
        return nullptr;
    }
    // ASIO's drivers take their class ID as the interface asked for.
    void* made = nullptr;
    const auto result = CoCreateInstance(classId, nullptr, CLSCTX_INPROC_SERVER, classId, &made);
    if (FAILED(result) || made == nullptr)
    {
        char code[16]{};
        std::snprintf(code, sizeof(code), "%08lX", static_cast<unsigned long>(result));
        problem = std::string("Windows couldn't load the driver (") + code + ").";
        return nullptr;
    }
    return static_cast<IASIO*>(made);
}
}

Host installedDrivers()
{
    Host host;
    host.drivers = registryDrivers;
    host.make = comDriver;
    return host;
}

std::shared_ptr<AudioSystem> makeSystem(Host host)
{
    return std::make_shared<AsioSystem>(std::move(host));
}

}
