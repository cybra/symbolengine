// ASIO's lessons against fake drivers the tests make in place of the registry's: listing loads nothing and opening
// loads one, every call made from one thread of the module's own in ASIO's order of states, drivers that crash, never
// return or write past what they're given, callbacks outside the run, the input of each buffer switch reaching the
// program in that cycle, gaps from the sample position, resets, rates said and refused, counts that change with the
// rate, sample types converted exactly, and a driver's preferred size kept. With SYMBOLENGINE_REAL_DEVICES=1 the PC's
// own drivers are listed, and with SYMBOLENGINE_TEST_ASIO naming part of one, its first two inputs are recorded,
// nothing played through it.

#include "AsioSystem.h"
#include "Helpers.h"

#include <symbolengine/core/System.h>
#include <symbolengine/devices/AudioDevices.h>

#ifndef NOMINMAX
    #define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>

#include <asiosys.h>
#include <asio.h>
#include <iasiodrv.h>

#include <catch2/catch_test_macros.hpp>

// COM's interfaces have no virtual destructors, by design.
#if defined(__GNUC__)
    #pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace symbolengine;
using Kind = AudioEvent::Kind;

namespace
{
struct FakeSettings
{
    long inputs = 2, outputs = 2;
    std::vector<ASIOSampleType> types{};  // each channel's, inputs first; Int32LSB where none is given
    long smallest = 64, largest = 2048, preferred = 512, granularity = -1;
    double rate = 48000.0;
    std::vector<double> rates{44100.0, 48000.0, 96000.0};
    std::map<double, std::pair<long, long>> channelsAt{};  // channel counts once set to a rate
    bool sayYesToRatesButStay = false;
    bool onlyPreferredSize = false;
    long initResult = 1;
    std::string errorMessage = "No interface is connected";
    std::string crashIn{}, hangIn{};
    bool writePast = false;
    bool timeInfo = true;
    bool callBackWhileMaking = false;
    int callsAfterStop = 0;
    double speed = 1.0;
};

// A driver as a test makes it: its calls kept with the thread each came on, its states checked, its hardware a thread
// switching buffers at its rate, and what a test injects given at the next switch.
class FakeDriver final : public IASIO
{
public:
    explicit FakeDriver(FakeSettings chosen) : settings(std::move(chosen)), rate(settings.rate) {}

    ~FakeDriver()
    {
        stopHardware();
        freeBuffers();
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override
    {
        *out = this;
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(++references); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        enter("Release");
        if (state != State::loaded && state != State::initialized)
            ++violations;
        released = true;
        return static_cast<ULONG>(--references);
    }

    ASIOBool init(void* window) override
    {
        enter("init");
        if (state != State::loaded)
            ++violations;
        given.window = window != nullptr;
        if (settings.initResult > 0)
            state = State::initialized;
        return settings.initResult;
    }

    void getDriverName(char* name) override
    {
        enter("getDriverName");
        writeText(name, "Fake ASIO", 32);
    }

    long getDriverVersion() override { return 1; }

    void getErrorMessage(char* text) override
    {
        enter("getErrorMessage");
        writeText(text, settings.errorMessage, 124);
    }

    ASIOError start() override
    {
        enter("start");
        if (state != State::prepared)
            ++violations;
        state = State::running;
        startHardware();
        return ASE_OK;
    }

    ASIOError stop() override
    {
        enter("stop");
        if (state != State::running)
            ++violations;
        stopHardware();
        state = State::prepared;
        return ASE_OK;
    }

    ASIOError getChannels(long* in, long* out) override
    {
        enter("getChannels");
        initialized();
        *in = settings.inputs;
        *out = settings.outputs;
        return ASE_OK;
    }

    ASIOError getLatencies(long* in, long* out) override
    {
        enter("getLatencies");
        initialized();
        *in = size + 32;
        *out = size + 48;
        return ASE_OK;
    }

    ASIOError getBufferSize(long* smallest, long* largest, long* preferred, long* granularity) override
    {
        enter("getBufferSize");
        initialized();
        *smallest = settings.smallest;
        *largest = settings.largest;
        *preferred = preferredSize.load();
        *granularity = settings.granularity;
        return ASE_OK;
    }

    ASIOError canSampleRate(ASIOSampleRate asked) override
    {
        enter("canSampleRate");
        initialized();
        return std::find(settings.rates.begin(), settings.rates.end(), asked) != settings.rates.end() ? ASE_OK
                                                                                                      : ASE_NoClock;
    }

    ASIOError getSampleRate(ASIOSampleRate* now) override
    {
        enter("getSampleRate");
        initialized();
        *now = rate.load();
        return ASE_OK;
    }

    ASIOError setSampleRate(ASIOSampleRate asked) override
    {
        enter("setSampleRate");
        initialized();
        ++rateSets;
        if (std::find(settings.rates.begin(), settings.rates.end(), asked) == settings.rates.end())
            return ASE_NoClock;
        if (!settings.sayYesToRatesButStay)
        {
            rate = asked;
            settings.rate = asked;
            if (const auto counts = settings.channelsAt.find(asked); counts != settings.channelsAt.end())
            {
                settings.inputs = counts->second.first;
                settings.outputs = counts->second.second;
            }
        }
        return ASE_OK;
    }

    ASIOError getClockSources(ASIOClockSource* clocks, long* count) override
    {
        enter("getClockSources");
        clocks[0] = {};
        *count = 1;
        return ASE_OK;
    }

    ASIOError setClockSource(long) override
    {
        enter("setClockSource");
        return ASE_OK;
    }

    ASIOError getSamplePosition(ASIOSamples* at, ASIOTimeStamp* time) override
    {
        enter("getSamplePosition");
        at->hi = 0;
        at->lo = static_cast<unsigned long>(position);
        *time = {};
        return ASE_OK;
    }

    ASIOError getChannelInfo(ASIOChannelInfo* info) override
    {
        enter("getChannelInfo");
        initialized();
        const auto index = info->isInput ? info->channel : settings.inputs + info->channel;
        if (info->channel < 0 || (info->isInput && info->channel >= settings.inputs) ||
            (!info->isInput && info->channel >= settings.outputs))
            return ASE_InvalidParameter;
        info->type = typeOf(index);
        info->isActive = ASIOFalse;
        info->channelGroup = 0;
        const auto label = std::string(info->isInput ? "In " : "Out ") + std::to_string(info->channel + 1);
        if (settings.writePast)
        {
            std::memset(info->name, 'N', sizeof(info->name));
            std::memset(reinterpret_cast<char*>(info) + sizeof(ASIOChannelInfo), 0x5A, 1024);
        }
        else
            writeText(info->name, label, 32);
        return ASE_OK;
    }

    ASIOError createBuffers(ASIOBufferInfo* infos, long count, long frames, ASIOCallbacks* callbacks) override
    {
        enter("createBuffers");
        if (state != State::initialized)
            ++violations;
        sizesMade.push_back(frames);
        if (settings.onlyPreferredSize && frames != preferredSize.load())
            return ASE_InvalidMode;
        size = frames;
        host = callbacks;
        timeInfoMode = settings.timeInfo && callbacks->asioMessage(kAsioSupportsTimeInfo, 0, nullptr, nullptr) == 1;
        freeBuffers();
        for (long i = 0; i < count; ++i)
        {
            const auto index = infos[i].isInput ? infos[i].channelNum : settings.inputs + infos[i].channelNum;
            Made made;
            made.input = infos[i].isInput == ASIOTrue;
            made.channel = infos[i].channelNum;
            made.bytes = bytesOf(typeOf(index));
            for (auto& half : made.halves)
                half.assign(static_cast<std::size_t>(frames * made.bytes), 0xEE);
            infos[i].buffers[0] = made.halves[0].data();
            infos[i].buffers[1] = made.halves[1].data();
            buffers.push_back(std::move(made));
        }
        if (settings.writePast)
            std::memset(infos + count, 0x5A, 1024);
        state = State::prepared;
        if (settings.callBackWhileMaking)
            switchBuffers(0);
        return ASE_OK;
    }

    ASIOError disposeBuffers() override
    {
        enter("disposeBuffers");
        if (state != State::prepared)
            ++violations;
        freeBuffers();
        state = State::initialized;
        return ASE_OK;
    }

    ASIOError controlPanel() override
    {
        enter("controlPanel");
        return ASE_OK;
    }

    ASIOError future(long, void*) override { return ASE_NotPresent; }

    // Called from the driver's own thread too, in its callbacks, as ASIO has it, so its thread isn't kept.
    ASIOError outputReady() override
    {
        ++readies;
        return ASE_OK;
    }

    // What the test sees and does.

    std::vector<std::string> calls() const
    {
        const std::lock_guard hold(lock);
        return callNames;
    }

    std::set<DWORD> threads() const
    {
        const std::lock_guard hold(lock);
        return callThreads;
    }

    // Each input buffer's frames, from a test, as the driver's own bytes; empty for the switch count in every frame.
    void feed(long channel, std::vector<unsigned char> bytes)
    {
        const std::lock_guard hold(lock);
        fed[channel] = std::move(bytes);
    }

    std::vector<unsigned char> lastOutput(long channel) const
    {
        const std::lock_guard hold(lock);
        const auto found = outputSeen.find(channel);
        return found != outputSeen.end() ? found->second : std::vector<unsigned char>{};
    }

    // Before the next switch: the position jumps, a message is sent, or a rate is said.
    std::atomic<long> jump{0}, message{0};
    std::atomic<double> notice{-1.0};
    std::atomic<long> preferredSize{512};

    FakeSettings settings;
    std::atomic<int> violations{0}, readies{0}, rateSets{0};
    std::atomic<bool> released{false}, notInApartment{false};
    std::atomic<std::int64_t> switches{0}, switchesAfterStop{0};
    std::atomic<std::int64_t> switchingNow{0};
    std::vector<long> sizesMade;
    struct
    {
        bool window = false;
    } given;

private:
    enum class State
    {
        loaded,
        initialized,
        prepared,
        running
    };

    struct Made
    {
        bool input = false;
        long channel = 0;
        int bytes = 4;
        std::array<std::vector<unsigned char>, 2> halves;
    };

    void enter(const char* name)
    {
        {
            const std::lock_guard hold(lock);
            callNames.push_back(name);
            callThreads.insert(GetCurrentThreadId());
        }
        APTTYPE type{};
        APTTYPEQUALIFIER qualifier{};
        if (FAILED(CoGetApartmentType(&type, &qualifier)) || (type != APTTYPE_STA && type != APTTYPE_MAINSTA))
            notInApartment = true;
        if (settings.crashIn == name)
            RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
        if (settings.hangIn == name)
            Sleep(3000);
    }

    void initialized()
    {
        if (state == State::loaded)
            ++violations;
    }

    ASIOSampleType typeOf(long index) const
    {
        if (index < static_cast<long>(settings.types.size()))
            return settings.types[static_cast<std::size_t>(index)];
        return ASIOSTInt32LSB;
    }

    static int bytesOf(ASIOSampleType type)
    {
        switch (type)
        {
            case ASIOSTInt16LSB:
            case ASIOSTInt16MSB: return 2;
            case ASIOSTInt24LSB:
            case ASIOSTInt24MSB: return 3;
            case ASIOSTFloat64LSB:
            case ASIOSTFloat64MSB: return 8;
            default: return 4;
        }
    }

    static void writeText(char* to, const std::string& text, std::size_t room)
    {
        std::memset(to, 0, room);
        std::memcpy(to, text.data(), std::min(text.size(), room - 1));
    }

    void freeBuffers()
    {
        const std::lock_guard hold(lock);
        buffers.clear();
    }

    void startHardware()
    {
        ending = false;
        hardware = std::thread([this] {
            const auto began = std::chrono::steady_clock::now();
            std::int64_t delivered = 0;
            long half = 0;
            while (!ending.load())
            {
                const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
                const auto due = static_cast<std::int64_t>(seconds * rate.load() * settings.speed / static_cast<double>(size));
                while (delivered < due && !ending.load())
                {
                    switchBuffers(half);
                    half ^= 1;
                    ++delivered;
                }
                std::this_thread::sleep_for(1ms);
            }
            // Drivers that keep calling back after they are stopped.
            for (int i = 0; i < settings.callsAfterStop; ++i)
            {
                switchBuffers(half);
                half ^= 1;
                ++switchesAfterStop;
            }
        });
    }

    void stopHardware()
    {
        ending = true;
        if (hardware.joinable())
            hardware.join();
    }

    void switchBuffers(long half)
    {
        const auto count = ++switches;
        switchingNow = count;
        if (const auto skipped = jump.exchange(0); skipped != 0)
            position += skipped;
        if (const auto selector = message.exchange(0); selector != 0)
            host->asioMessage(selector, 0, nullptr, nullptr);
        if (const auto rateSaid = notice.exchange(-1.0); rateSaid >= 0.0)
            host->sampleRateDidChange(rateSaid);

        {
            const std::lock_guard hold(lock);
            for (auto& buffer : buffers)
            {
                if (!buffer.input)
                    continue;
                auto& bytes = buffer.halves[static_cast<std::size_t>(half)];
                if (const auto found = fed.find(buffer.channel); found != fed.end() && !found->second.empty())
                {
                    for (std::size_t at = 0; at + found->second.size() <= bytes.size(); at += found->second.size())
                        std::memcpy(bytes.data() + at, found->second.data(), found->second.size());
                }
                else
                {
                    // The switch's count in every frame, as a 32-bit whole number, so a program sees which switch
                    // its input came from.
                    const auto stamp = static_cast<std::int32_t>(count << 8);
                    for (std::size_t at = 0; at + 4 <= bytes.size(); at += 4)
                        std::memcpy(bytes.data() + at, &stamp, 4);
                }
            }
        }

        if (timeInfoMode)
        {
            ASIOTime time{};
            time.timeInfo.flags = kSystemTimeValid | kSamplePositionValid | kSampleRateValid;
            time.timeInfo.samplePosition.hi = static_cast<unsigned long>(static_cast<std::uint64_t>(position) >> 32);
            time.timeInfo.samplePosition.lo = static_cast<unsigned long>(static_cast<std::uint64_t>(position) & 0xFFFFFFFF);
            time.timeInfo.sampleRate = rate.load();
            host->bufferSwitchTimeInfo(&time, half, ASIOTrue);
        }
        else
            host->bufferSwitch(half, ASIOTrue);
        position += size;

        const std::lock_guard hold(lock);
        for (auto& buffer : buffers)
            if (!buffer.input)
                outputSeen[buffer.channel] = buffer.halves[static_cast<std::size_t>(half)];
    }

    mutable std::mutex lock;
    std::vector<std::string> callNames;
    std::set<DWORD> callThreads;
    std::map<long, std::vector<unsigned char>> fed, outputSeen;
    std::atomic<long> references{1};
    std::atomic<State> state{State::loaded};
    std::atomic<double> rate;
    long size = 512;
    std::int64_t position = 0;
    ASIOCallbacks* host = nullptr;
    bool timeInfoMode = false;
    std::vector<Made> buffers;
    std::thread hardware;
    std::atomic<bool> ending{false};
};

// The drivers a test installs: listed as the registry would list them, made when the ASIO layer loads one.
class TestDrivers
{
public:
    void add(const std::string& id, const std::string& name, FakeSettings settings = {}, std::string problem = {})
    {
        listed.push_back({id, name, std::move(problem)});
        settingsOf[id] = std::move(settings);
    }

    asio::detail::Host host(std::chrono::milliseconds callTimeout = 1000ms)
    {
        asio::detail::Host made;
        made.drivers = [this] { return listed; };
        made.make = [this](const std::string& id, std::string& problem) -> IASIO* {
            const std::lock_guard hold(lock);
            ++loads;
            const auto found = settingsOf.find(id);
            if (found == settingsOf.end())
            {
                problem = "No such driver.";
                return nullptr;
            }
            // The hardware keeps its rate, channels and the size chosen in its panel from one load to the next.
            auto& loaded = drivers[id];
            auto driver = std::make_shared<FakeDriver>(loaded.empty() ? found->second : loaded.back()->settings);
            driver->preferredSize = loaded.empty() ? found->second.preferred : loaded.back()->preferredSize.load();
            loaded.push_back(driver);
            return driver.get();
        };
        made.callTimeout = callTimeout;
        return made;
    }

    // The driver made last for an id, and how many have been made.
    std::shared_ptr<FakeDriver> last(const std::string& id)
    {
        const std::lock_guard hold(lock);
        const auto found = drivers.find(id);
        return found != drivers.end() && !found->second.empty() ? found->second.back() : nullptr;
    }

    int made(const std::string& id)
    {
        const std::lock_guard hold(lock);
        const auto found = drivers.find(id);
        return found != drivers.end() ? static_cast<int>(found->second.size()) : 0;
    }

    std::atomic<int> loads{0};

private:
    std::vector<asio::detail::Installed> listed;
    std::map<std::string, FakeSettings> settingsOf;
    std::mutex lock;
    std::map<std::string, std::vector<std::shared_ptr<FakeDriver>>> drivers;
};

AudioDevices::Options optionsFor(TestDrivers& installed, Events& events, std::chrono::milliseconds callTimeout = 1000ms)
{
    AudioDevices::Options options;
    options.systems = {asio::detail::makeSystem(installed.host(callTimeout))};
    options.onEvent = events.handler();
    options.describeTimeout = 300ms;
    options.settle = 20ms;
    options.streams = quick(events);
    options.streams.callTimeout = 5000ms;
    return options;
}

// A copy of the device listed with an identifier, since a list given back by value is gone after the statement.
std::optional<AudioDeviceInfo> listedAs(const std::vector<AudioDeviceInfo>& devices, const std::string& id)
{
    const auto found = std::find_if(devices.begin(), devices.end(), [&](const auto& d) { return d.id == id; });
    if (found == devices.end())
        return std::nullopt;
    return *found;
}

// Every call the driver had, on one thread, an apartment of its own with a window, in ASIO's order.
void checkCalledProperly(const FakeDriver& driver)
{
    CHECK(driver.threads().size() == 1);
    CHECK_FALSE(driver.notInApartment.load());
    CHECK(driver.violations.load() == 0);
    CHECK(driver.given.window);
}
}

TEST_CASE("Listing ASIO drivers loads none, and a stream loads its driver once, on one thread of its own, in order")
{
    TestDrivers installed;
    installed.add("{A}", "Interface ASIO");
    installed.add("{B}", "Old ASIO", {}, "The driver is built for another kind of processor than this program's.");
    installed.add("{C}", "Other ASIO");
    Events events;
    AudioDevices devices(optionsFor(installed, events));

    const auto list = devices.devices("ASIO");
    REQUIRE(list.size() == 3);
    CHECK(installed.loads.load() == 0);
    CHECK(list[0].unread);
    CHECK(list[0].name == "Interface ASIO");
    CHECK_FALSE(list[1].answering);
    CHECK(devices.defaultDevice("ASIO", AudioDirection::output).empty());

    Recording recording(1 << 16);
    AudioError error;
    auto stream = devices.open("ASIO", "{A}", stereo(), error);
    REQUIRE(stream != nullptr);
    REQUIRE(!stream->add(recording));
    REQUIRE(!stream->start());
    REQUIRE(soon([&] { return recording.frames > 48000; }));
    const auto info = stream->info();
    REQUIRE(!stream->stop());
    stream.reset();

    CHECK(installed.loads.load() == 1);
    CHECK(installed.made("{C}") == 0);
    const auto driver = installed.last("{A}");
    REQUIRE(driver != nullptr);
    checkCalledProperly(*driver);
    CHECK(driver->released.load());
    CHECK(info.sampleRate == 48000.0);
    CHECK(info.maxBlock == 512);
    CHECK(info.inputChannels == 2);
    CHECK(info.outputChannels == 2);
    CHECK(info.inputLatency == 512 + 32);
    CHECK(info.outputLatency == 512 + 48);
    CHECK(recording.emptyBlocks == 0);
    CHECK(driver->readies.load() > 0);

    // Once loaded, the driver is listed with what it has, and the lists say so.
    REQUIRE(soon([&] {
        const auto now = devices.devices("ASIO");
        const auto a = listedAs(now, "{A}");
        return a.has_value() && !a->unread;
    }));
    const auto a = listedAs(devices.devices("ASIO"), "{A}");
    CHECK(a->inputNames == std::vector<std::string>{"In 1", "In 2"});
    CHECK(a->outputNames == std::vector<std::string>{"Out 1", "Out 2"});
    CHECK(a->sampleRates == std::vector<double>{44100.0, 48000.0, 96000.0});
    CHECK(a->blockSizes == std::vector<int>{512, 64, 128, 256, 1024, 2048});
    CHECK(a->currentRate == 48000.0);
}

TEST_CASE("A second ASIO driver is refused while one is open, and opens once the first is let go of")
{
    TestDrivers installed;
    installed.add("{A}", "First ASIO");
    installed.add("{B}", "Second ASIO");
    Events events;
    AudioDevices devices(optionsFor(installed, events));

    AudioError error;
    auto first = devices.open("ASIO", "{A}", stereo(false, true), error);
    REQUIRE(first != nullptr);
    REQUIRE(!first->start());
    auto second = devices.open("ASIO", "{B}", stereo(false, true), error);
    REQUIRE(second != nullptr);
    const auto refused = second->start();
    CHECK(refused.message == "ASIO lets a program hold one driver at a time, and another is open.");
    CHECK(installed.made("{B}") == 0);

    REQUIRE(!first->stop());
    first.reset();
    second.reset();
    auto again = devices.open("ASIO", "{B}", stereo(false, true), error);
    REQUIRE(again != nullptr);
    CHECK(!again->start());
    CHECK(installed.made("{B}") == 1);
}

TEST_CASE("A driver that crashes in any call is let go of and said, and the next driver opens as if nothing happened")
{
    for (const std::string call : {"init", "getChannels", "getChannelInfo", "getBufferSize", "getSampleRate",
                                   "createBuffers", "getLatencies", "start", "stop", "disposeBuffers", "Release"})
    {
        INFO("crashing in " << call);
        TestDrivers installed;
        FakeSettings crashing;
        crashing.crashIn = call;
        installed.add("{A}", "Crashing ASIO", crashing);
        installed.add("{B}", "Healthy ASIO");
        Events events;
        AudioDevices devices(optionsFor(installed, events));

        AudioError error;
        auto stream = devices.open("ASIO", "{A}", stereo(), error);
        REQUIRE(stream != nullptr);
        const auto started = stream->start();
        if (call == "stop" || call == "disposeBuffers" || call == "Release")
        {
            // A crash on the way out, or in what is read beside, ends nothing the program has.
            CHECK(!started);
            std::this_thread::sleep_for(50ms);
            CHECK(!stream->stop());
        }
        else
        {
            CHECK(started.message == "The ASIO driver crashed.");
            CHECK(stream->state() != AudioStream::State::running);
        }
        stream.reset();

        auto next = devices.open("ASIO", "{B}", stereo(), error);
        REQUIRE(next != nullptr);
        CHECK(!next->start());
        REQUIRE(!next->stop());
        next.reset();
        const auto healthy = installed.last("{B}");
        REQUIRE(healthy != nullptr);
        checkCalledProperly(*healthy);
        CHECK(healthy->released.load());
    }
}

TEST_CASE("A driver that never returns from a call is said not to answer within the bound, and the next one opens")
{
    TestDrivers installed;
    FakeSettings hanging;
    hanging.hangIn = "getChannels";
    installed.add("{A}", "Hanging ASIO", hanging);
    installed.add("{B}", "Healthy ASIO");
    Events events;
    AudioDevices devices(optionsFor(installed, events, 300ms));

    AudioError error;
    auto stream = devices.open("ASIO", "{A}", stereo(), error);
    REQUIRE(stream != nullptr);
    const auto began = std::chrono::steady_clock::now();
    const auto started = stream->start();
    CHECK(started.message == "The ASIO driver stopped answering.");
    CHECK(std::chrono::steady_clock::now() - began < 2s);
    stream.reset();

    auto next = devices.open("ASIO", "{B}", stereo(), error);
    REQUIRE(next != nullptr);
    CHECK(!next->start());
    REQUIRE(!next->stop());

    // The driver held up returns at last, into a load that is gone, before the test lets go of the fake.
    std::this_thread::sleep_for(3200ms);
}

TEST_CASE("A driver that fails to start says why, in its own words")
{
    TestDrivers installed;
    FakeSettings failing;
    failing.initResult = 0;
    installed.add("{A}", "Unplugged ASIO", failing);
    Events events;
    AudioDevices devices(optionsFor(installed, events));

    AudioError error;
    auto stream = devices.open("ASIO", "{A}", stereo(), error);
    REQUIRE(stream != nullptr);
    const auto started = stream->start();
    CHECK(started.message == "The ASIO driver didn't start: No interface is connected.");
    CHECK(started.step == "init");
    stream.reset();
    const auto driver = installed.last("{A}");
    REQUIRE(driver != nullptr);
    checkCalledProperly(*driver);
    CHECK(driver->released.load());
}

TEST_CASE("A driver writing a kilobyte past what it's given corrupts nothing, and its unended names are bounded")
{
    TestDrivers installed;
    FakeSettings careless;
    careless.writePast = true;
    installed.add("{A}", "Careless ASIO", careless);
    Events events;
    AudioDevices devices(optionsFor(installed, events));

    Recording recording(1 << 16);
    AudioError error;
    auto stream = devices.open("ASIO", "{A}", stereo(), error);
    REQUIRE(stream != nullptr);
    REQUIRE(!stream->add(recording));
    REQUIRE(!stream->start());
    REQUIRE(soon([&] { return recording.frames > 4800; }));
    REQUIRE(!stream->stop());
    stream.reset();

    REQUIRE(soon([&] {
        const auto a = listedAs(devices.devices("ASIO"), "{A}");
        return a.has_value() && !a->unread;
    }));
    const auto a = listedAs(devices.devices("ASIO"), "{A}");
    CHECK(a->inputNames == std::vector<std::string>(2, std::string(32, 'N')));
    checkCalledProperly(*installed.last("{A}"));
}

TEST_CASE("Callbacks while the buffers are made, before the start and after the stop reach nothing of the program's")
{
    TestDrivers installed;
    FakeSettings early;
    early.callBackWhileMaking = true;
    early.callsAfterStop = 20;
    installed.add("{A}", "Eager ASIO", early);
    Events events;
    AudioDevices devices(optionsFor(installed, events));

    Recording recording(1 << 16);
    AudioError error;
    auto stream = devices.open("ASIO", "{A}", stereo(), error);
    REQUIRE(stream != nullptr);
    REQUIRE(!stream->add(recording));
    REQUIRE(!stream->start());
    REQUIRE(soon([&] { return recording.frames > 4800; }));
    REQUIRE(!stream->stop());
    const auto driver = installed.last("{A}");
    const auto framesAtStop = recording.frames.load();
    const auto switches = driver->switches.load();
    stream.reset();

    CHECK(driver->switchesAfterStop.load() == 20);
    CHECK(recording.frames.load() == framesAtStop);
    // Every frame the program had came from a switch of the run, none from the one made with the buffers.
    CHECK(framesAtStop <= (switches - 1 - 20) * 512);
    // What the driver replays after its stop is silence.
    CHECK(driver->lastOutput(0) == std::vector<unsigned char>(512 * 4, 0));
    checkCalledProperly(*driver);
}

TEST_CASE("The input of each buffer switch reaches the program in that same switch")
{
    TestDrivers installed;
    installed.add("{A}", "Interface ASIO");
    Events events;
    AudioDevices devices(optionsFor(installed, events));

    Recording recording(1 << 16);
    std::atomic<FakeDriver*> driver{nullptr};
    std::atomic<int> late{0}, checked{0};
    recording.onProcess = [&](const AudioBlock& block) {
        auto* fake = driver.load();
        if (fake == nullptr || block.inputChannels == 0)
            return;
        const auto stamp = static_cast<double>(fake->switchingNow.load() << 8) / 2147483648.0;
        if (static_cast<double>(block.inputs[0][0]) != static_cast<double>(static_cast<float>(stamp)))
            ++late;
        ++checked;
    };
    AudioError error;
    auto stream = devices.open("ASIO", "{A}", stereo(true, false), error);
    REQUIRE(stream != nullptr);
    REQUIRE(!stream->add(recording));
    REQUIRE(!stream->start());
    const auto made = installed.last("{A}");
    driver = made.get();
    REQUIRE(soon([&] { return checked.load() > 50; }));
    REQUIRE(!stream->stop());
    CHECK(late.load() == 0);
}

TEST_CASE("A jump in the sample position is a dropout of its length; an overload is one only where there is no position")
{
    SECTION("with the time information")
    {
        TestDrivers installed;
        installed.add("{A}", "Interface ASIO");
        Events events;
        AudioDevices devices(optionsFor(installed, events));

        Recording recording(1 << 16);
        AudioError error;
        auto stream = devices.open("ASIO", "{A}", stereo(true, false), error);
        REQUIRE(stream != nullptr);
        REQUIRE(!stream->add(recording));
        REQUIRE(!stream->start());
        REQUIRE(soon([&] { return recording.frames > 4800; }));
        const auto driver = installed.last("{A}");
        driver->jump = 1024;
        REQUIRE(events.waitFor(Kind::dropout));
        const auto dropout = events.of(Kind::dropout).front();
        CHECK(dropout.frames == 1024);

        // The positions run on, so the overload said cost nothing the timeline shows.
        driver->message = kAsioOverload;
        REQUIRE(soon([&] { return driver->message.load() == 0; }));
        std::this_thread::sleep_for(100ms);
        REQUIRE(!stream->stop());
        CHECK(events.count(Kind::dropout) == 1);
    }

    SECTION("with no time information")
    {
        TestDrivers installed;
        FakeSettings plain;
        plain.timeInfo = false;
        installed.add("{A}", "Plain ASIO", plain);
        Events events;
        AudioDevices devices(optionsFor(installed, events));

        Recording recording(1 << 16);
        AudioError error;
        auto stream = devices.open("ASIO", "{A}", stereo(true, false), error);
        REQUIRE(stream != nullptr);
        REQUIRE(!stream->add(recording));
        REQUIRE(!stream->start());
        REQUIRE(soon([&] { return recording.frames > 4800; }));
        const auto driver = installed.last("{A}");
        driver->message = kAsioOverload;
        REQUIRE(events.waitFor(Kind::dropout));
        REQUIRE(!stream->stop());
        CHECK(events.of(Kind::dropout).front().frames == -1);
    }
}

TEST_CASE("A driver asking to reset is opened again once, at the size it now prefers, and the change is said")
{
    TestDrivers installed;
    installed.add("{A}", "Interface ASIO");
    Events events;
    AudioDevices devices(optionsFor(installed, events));

    Recording recording(1 << 16);
    AudioError error;
    auto stream = devices.open("ASIO", "{A}", stereo(), error);
    REQUIRE(stream != nullptr);
    REQUIRE(!stream->add(recording));
    REQUIRE(!stream->start());
    REQUIRE(soon([&] { return recording.frames > 4800; }));

    // The user chose 256 in the driver's own panel, which then asks to reset, twice.
    auto first = installed.last("{A}");
    first->preferredSize = 256;
    first->message = kAsioResetRequest;
    REQUIRE(soon([&] { return first->message.load() == 0; }));
    first->message = kAsioResetRequest;
    REQUIRE(events.waitFor(Kind::formatChanged));
    std::this_thread::sleep_for(200ms);
    CHECK(events.count(Kind::formatChanged) == 1);
    CHECK(installed.made("{A}") == 2);
    const auto change = events.of(Kind::formatChanged).front();
    CHECK(change.before.maxBlock == 512);
    CHECK(change.after.maxBlock == 256);
    CHECK(stream->state() == AudioStream::State::running);
    REQUIRE(!stream->stop());
    stream.reset();
    CHECK(first->released.load());
    checkCalledProperly(*first);
    checkCalledProperly(*installed.last("{A}"));
    CHECK(installed.last("{A}")->sizesMade == std::vector<long>{256});
}

TEST_CASE("A rate the driver says yes to but doesn't take is refused, its own rate said again passed over, and 0 lost")
{
    SECTION("said yes to and not taken")
    {
        TestDrivers installed;
        FakeSettings stubborn;
        stubborn.sayYesToRatesButStay = true;
        installed.add("{A}", "Stubborn ASIO", stubborn);
        Events events;
        AudioDevices devices(optionsFor(installed, events));
        AudioError error;
        auto request = stereo();
        request.sampleRate = 96000.0;
        auto stream = devices.open("ASIO", "{A}", request, error);
        REQUIRE(stream != nullptr);
        const auto started = stream->start();
        CHECK(started.message == "The ASIO driver said it changed to that rate, but runs at another.");
    }

    SECTION("its own rate said again, then no rate at all")
    {
        TestDrivers installed;
        installed.add("{A}", "Interface ASIO");
        Events events;
        AudioDevices devices(optionsFor(installed, events));
        Recording recording(1 << 16);
        AudioError error;
        auto stream = devices.open("ASIO", "{A}", stereo(), error);
        REQUIRE(stream != nullptr);
        REQUIRE(!stream->add(recording));
        REQUIRE(!stream->start());
        REQUIRE(soon([&] { return recording.frames > 4800; }));
        const auto driver = installed.last("{A}");
        driver->notice = 48000.0;
        REQUIRE(soon([&] { return driver->notice.load() < 0.0; }));
        std::this_thread::sleep_for(200ms);
        CHECK(installed.made("{A}") == 1);
        CHECK(events.count(Kind::formatChanged) == 0);

        driver->notice = 0.0;
        REQUIRE(events.waitFor(Kind::lost));
        CHECK(events.of(Kind::lost).front().loss == AudioLoss::clockLost);
    }
}

TEST_CASE("A rate set is read back, and the channels read again at it, as a device's channels can depend on its rate")
{
    TestDrivers installed;
    FakeSettings adat;
    adat.inputs = 16;
    adat.outputs = 16;
    adat.channelsAt[96000.0] = {8, 8};
    installed.add("{A}", "Interface ASIO", adat);
    Events events;
    AudioDevices devices(optionsFor(installed, events));

    AudioError error;
    AudioStreamRequest request;
    request.inputChannels = {12};
    request.sampleRate = 96000.0;
    auto stream = devices.open("ASIO", "{A}", request, error);
    REQUIRE(stream != nullptr);
    CHECK(stream->start().message == "The ASIO driver doesn't have that input channel.");
    stream.reset();

    request.inputChannels = {7};
    stream = devices.open("ASIO", "{A}", request, error);
    REQUIRE(stream != nullptr);
    REQUIRE(!stream->start());
    CHECK(stream->info().sampleRate == 96000.0);
    REQUIRE(soon([&] {
        const auto a = listedAs(devices.devices("ASIO"), "{A}");
        return a.has_value() && a->inputs() == 8;
    }));
}

TEST_CASE("Each channel's sample type is converted on its own, exactly, both ways")
{
    TestDrivers installed;
    FakeSettings mixed;
    mixed.types = {ASIOSTInt24LSB, ASIOSTInt16MSB, ASIOSTFloat32LSB, ASIOSTInt32LSB24};
    installed.add("{A}", "Mixed ASIO", mixed);
    Events events;
    AudioDevices devices(optionsFor(installed, events));

    Recording recording(1 << 16);
    std::atomic<float> first{0.0f}, second{0.0f};
    recording.onProcess = [&](const AudioBlock& block) {
        first = block.inputs[0][0];
        second = block.inputs[1][0];
        for (int c = 0; c < block.outputChannels; ++c)
            std::fill(block.outputs[c], block.outputs[c] + block.frames, 0.25f);
    };
    AudioError error;
    auto stream = devices.open("ASIO", "{A}", stereo(), error);
    REQUIRE(stream != nullptr);
    REQUIRE(!stream->add(recording));
    REQUIRE(!stream->start());
    auto fake = installed.last("{A}");
    // A 24-bit -1 step in three bytes, and a 16-bit 0.5 big-endian.
    fake->feed(0, {0xFF, 0xFF, 0xFF});
    fake->feed(1, {0x40, 0x00});
    const auto stepDown = static_cast<float>(-1.0 / 8388608.0);
    CHECK(soon([&] { return first.load() == stepDown && second.load() == 0.5f; }));
    const std::vector<unsigned char> quarter{0x00, 0x00, 0x80, 0x3E};
    CHECK(soon([&] {
        const auto out = fake->lastOutput(0);
        return out.size() >= 4 && std::vector<unsigned char>(out.begin(), out.begin() + 4) == quarter;
    }));
    REQUIRE(!stream->stop());

    const auto out = fake->lastOutput(0);
    REQUIRE(out.size() >= 4);
    float value = 0.0f;
    std::memcpy(&value, out.data(), 4);
    CHECK(value == 0.25f);
    const auto low = fake->lastOutput(1);
    REQUIRE(low.size() >= 4);
    CHECK(std::vector<unsigned char>(low.begin(), low.begin() + 4) == std::vector<unsigned char>{0x00, 0x00, 0x20, 0x00});
}

TEST_CASE("Ten opens of a driver all take its preferred size, never one of the module's own")
{
    TestDrivers installed;
    FakeSettings sizes;
    sizes.smallest = 8;
    sizes.largest = 1024;
    sizes.preferred = 32;
    sizes.granularity = 16;
    installed.add("{A}", "Interface ASIO", sizes);
    Events events;
    AudioDevices devices(optionsFor(installed, events));

    for (int i = 0; i < 10; ++i)
    {
        AudioError error;
        auto stream = devices.open("ASIO", "{A}", stereo(false, true), error);
        REQUIRE(stream != nullptr);
        REQUIRE(!stream->start());
        CHECK(stream->info().maxBlock == 32);
        REQUIRE(!stream->stop());
    }
    CHECK(installed.made("{A}") == 10);
    CHECK(installed.last("{A}")->sizesMade == std::vector<long>{32});
}

TEST_CASE("A size the driver won't take is tried once more at its preferred size")
{
    TestDrivers installed;
    FakeSettings picky;
    picky.onlyPreferredSize = true;
    installed.add("{A}", "Picky ASIO", picky);
    Events events;
    AudioDevices devices(optionsFor(installed, events));

    AudioError error;
    auto request = stereo(false, true);
    request.blockSize = 128;
    auto stream = devices.open("ASIO", "{A}", request, error);
    REQUIRE(stream != nullptr);
    REQUIRE(!stream->start());
    CHECK(stream->info().maxBlock == 512);
    CHECK(installed.last("{A}")->sizesMade == std::vector<long>{128, 512});
    checkCalledProperly(*installed.last("{A}"));
}

TEST_CASE("This PC's ASIO drivers are listed unread, and two seconds from the one named come in at its rate and live")
{
    if (system::environmentVariable("SYMBOLENGINE_REAL_DEVICES").value_or("") != "1")
        SKIP("SYMBOLENGINE_REAL_DEVICES=1 lists the PC's own ASIO drivers");

    Events events;
    AudioDevices::Options options;
    options.systems = {asio::detail::makeSystem()};
    options.onEvent = events.handler();
    AudioDevices devices(options);
    const auto list = devices.devices("ASIO");
    REQUIRE_FALSE(list.empty());
    for (const auto& driver : list)
    {
        INFO(driver.name << " " << driver.id << ": " << driver.problem);
        CHECK(driver.unread);
        CHECK(driver.inputs() == 0);
    }

    const auto part = system::environmentVariable("SYMBOLENGINE_TEST_ASIO").value_or("");
    if (part.empty())
        SKIP("SYMBOLENGINE_TEST_ASIO names part of the driver to record from");
    // The driver of exactly that name, or else the first whose name holds it, as wrappers name the drivers they wrap.
    auto chosen = std::find_if(list.begin(), list.end(), [&](const auto& d) { return d.name == part; });
    if (chosen == list.end())
        chosen = std::find_if(list.begin(), list.end(), [&](const auto& d) { return d.name.find(part) != std::string::npos; });
    REQUIRE(chosen != list.end());
    const auto id = chosen->id;

    // The rate is measured from a third of a second in, as a driver may give its first blocks in a burst.
    Recording recording(1 << 18);
    std::atomic<std::int64_t> firstTime{-1}, firstFrames{0}, lastTime{-1}, lastFrames{0}, before{0};
    std::atomic<std::int64_t> expected{-1}, zeros{0}, samples{0};
    std::atomic<bool> continuous{true};
    std::atomic<float> peak{0.0f};
    recording.onProcess = [&](const AudioBlock& block) {
        if (firstTime.load() < 0 && before.load() >= 16000)
        {
            firstTime = block.inputTime;
            firstFrames = before.load();
        }
        lastTime = block.inputTime;
        lastFrames = before.load();
        before += block.frames;
        if (expected.load() >= 0 && block.inputPosition != expected.load())
            continuous = false;
        expected = block.inputPosition + block.frames;
        auto loudest = peak.load();
        for (int c = 0; c < block.inputChannels; ++c)
            for (int i = 0; i < block.frames; ++i)
            {
                loudest = std::max(loudest, std::abs(block.inputs[c][i]));
                if (block.inputs[c][i] == 0.0f)
                    ++zeros;
            }
        peak = loudest;
        samples += static_cast<std::int64_t>(block.frames) * block.inputChannels;
    };

    AudioError error;
    AudioStreamRequest request;
    request.inputChannels = {0, 1};
    auto stream = devices.open("ASIO", id, request, error);
    REQUIRE(stream != nullptr);
    REQUIRE(!stream->add(recording));
    const auto started = stream->start();
    INFO(started.message);
    REQUIRE(!started);
    std::this_thread::sleep_for(2s);
    const auto info = stream->info();
    REQUIRE(!stream->stop());
    stream.reset();

    REQUIRE(soon([&] {
        const auto read = listedAs(devices.devices("ASIO"), id);
        return read.has_value() && !read->unread;
    }));
    const auto read = listedAs(devices.devices("ASIO"), id);
    INFO(read->name << ": " << read->inputs() << " inputs, the first " << read->inputNames.front() << ", " << read->outputs()
                    << " outputs; rate " << info.sampleRate << ", block " << info.maxBlock << ", input latency "
                    << info.inputLatency << ", " << recording.frames << " frames, peak "
                    << 20.0 * std::log10(std::max(peak.load(), 1e-12f)) << " dBFS, " << zeros << " samples exactly 0");
    CHECK(info.sampleRate == read->currentRate);
    CHECK(recording.emptyBlocks == 0);
    CHECK(continuous);
    CHECK(events.count(Kind::dropout) == 0);
    CHECK(events.count(Kind::stalled) == 0);
    CHECK(peak.load() > 0.0f);
    CHECK(zeros.load() < samples.load() / 2);
    CHECK(info.inputLatency >= info.maxBlock);

    // Its frames over the times of their blocks run at its rate, within half a percent.
    const auto seconds = static_cast<double>(lastTime - firstTime) / 1e9;
    REQUIRE(seconds > 1.2);
    const auto measured = static_cast<double>(lastFrames - firstFrames) / seconds;
    INFO("measured " << measured << " frames a second");
    CHECK(std::abs(measured - info.sampleRate) / info.sampleRate < 0.005);
}
