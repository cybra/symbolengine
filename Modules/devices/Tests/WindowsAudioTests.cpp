// Windows Audio's lessons on a real PC. With SYMBOLENGINE_REAL_DEVICES=1 they list its endpoints; with
// SYMBOLENGINE_TEST_OUTPUT and SYMBOLENGINE_TEST_INPUT naming part of an endpoint's name they play silence through
// that output and record from that input, in shared mode and exclusive, and another copy of these tests takes the
// input for its own while a stream records from it. Runners have no endpoints, so these are run by hand.

#include "Helpers.h"

#include <symbolengine/core/System.h>
#include <symbolengine/devices/AudioDevices.h>

#include <catch2/catch_test_macros.hpp>

#ifndef NOMINMAX
    #define NOMINMAX
#endif
#include <windows.h>

#include <mmdeviceapi.h>

// Whether an endpoint offers the raw stream, the key Windows 8.1 added, which the SDK's headers don't declare.
const PROPERTYKEY rawProcessingSupported{{0x8943b373, 0x388c, 0x4395, {0xb5, 0x57, 0xbc, 0x6d, 0xba, 0xff, 0xaf, 0xdb}}, 2};

#include <cmath>


using namespace symbolengine;

namespace
{
std::string setting(const char* name)
{
    return system::environmentVariable(name).value_or("");
}

const AudioDeviceInfo* named(const std::vector<AudioDeviceInfo>& devices, const std::string& part, bool input)
{
    for (const auto& device : devices)
        if (device.name.find(part) != std::string::npos && (input ? device.inputs() : device.outputs()) > 0)
            return &device;
    return nullptr;
}

// Whether Windows says an endpoint offers the raw stream, from its properties.
bool offersRaw(const std::string& id)
{
    const auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool offered = false;
    IMMDeviceEnumerator* enumerator = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                   reinterpret_cast<void**>(&enumerator))))
    {
        const std::wstring wide(id.begin(), id.end());
        IMMDevice* device = nullptr;
        if (SUCCEEDED(enumerator->GetDevice(wide.c_str(), &device)))
        {
            IPropertyStore* properties = nullptr;
            if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &properties)))
            {
                PROPVARIANT value;
                PropVariantInit(&value);
                if (SUCCEEDED(properties->GetValue(rawProcessingSupported, &value)) &&
                    value.vt == VT_BOOL)
                    offered = value.boolVal != VARIANT_FALSE;
                PropVariantClear(&value);
                properties->Release();
            }
            device->Release();
        }
        enumerator->Release();
    }
    if (SUCCEEDED(com))
        CoUninitialize();
    return offered;
}

AudioDevices::Options options(Events& events)
{
    AudioDevices::Options made;
    made.onEvent = events.handler();
    return made;
}
}

TEST_CASE("Windows Audio lists the PC's endpoints, each an input or an output, without opening any")
{
    if (setting("SYMBOLENGINE_REAL_DEVICES") != "1")
        SKIP("SYMBOLENGINE_REAL_DEVICES=1 lists the PC's own endpoints");

    Events events;
    AudioDevices devices(options(events));
#if defined(SYMBOLENGINE_ASIO)
    REQUIRE(devices.systems() == std::vector<std::string>{"Windows Audio", "ASIO"});
#else
    REQUIRE(devices.systems() == std::vector<std::string>{"Windows Audio"});
#endif
    const auto list = devices.devices("Windows Audio");
    REQUIRE_FALSE(list.empty());
    for (const auto& device : list)
    {
        INFO(device.name);
        CHECK_FALSE(device.id.empty());
        CHECK(device.answering);
        CHECK(((device.inputs() > 0) != (device.outputs() > 0)));
        CHECK(device.currentRate > 0.0);
    }
    CHECK_FALSE(devices.defaultDevice("Windows Audio", AudioDirection::output).empty());
}

TEST_CASE("Silence played for two seconds through the output named comes at the device's rate, nothing lost")
{
    const auto part = setting("SYMBOLENGINE_TEST_OUTPUT");
    if (part.empty())
        SKIP("SYMBOLENGINE_TEST_OUTPUT names the output to play silence through");

    Events events;
    AudioDevices devices(options(events));
    const auto list = devices.devices("Windows Audio");
    const auto* output = named(list, part, false);
    REQUIRE(output != nullptr);

    Recording recording;
    AudioError error;
    AudioStreamRequest request;
    request.outputChannels = {0, 1};
    auto stream = devices.open("Windows Audio", output->id, request, error);
    REQUIRE(stream != nullptr);
    REQUIRE(!stream->add(recording));
    const auto started = stream->start();
    INFO(started.message << " (" << started.step << ", " << started.code << ")");
    REQUIRE(!started);
    std::this_thread::sleep_for(2s);
    const auto info = stream->info();
    REQUIRE(!stream->stop());

    INFO(output->name << ": rate " << info.sampleRate << ", block " << info.maxBlock << ", " << recording.frames
                      << " frames, priority " << info.priority);
    CHECK(info.sampleRate == output->currentRate);
    CHECK(recording.largest <= info.maxBlock);
    CHECK(recording.emptyBlocks == 0);
    CHECK(events.count(AudioEvent::Kind::stalled) == 0);
    CHECK(std::abs(static_cast<double>(recording.frames) - 2.0 * info.sampleRate) < 0.15 * info.sampleRate);
    CHECK(info.priority.find("Pro Audio") != std::string::npos);
}

TEST_CASE("An input held up for 50 ms every two seconds loses nothing, its buffer longer than the hold")
{
    const auto part = setting("SYMBOLENGINE_TEST_INPUT");
    if (part.empty())
        SKIP("SYMBOLENGINE_TEST_INPUT names the input to record from");

    Events events;
    AudioDevices devices(options(events));
    const auto list = devices.devices("Windows Audio");
    const auto* input = named(list, part, true);
    REQUIRE(input != nullptr);

    Recording recording;
    std::atomic<std::int64_t> lastHold{0};
    std::atomic<int> holds{0};
    std::atomic<bool> continuous{true};
    std::atomic<std::int64_t> expected{-1};
    recording.onProcess = [&](const AudioBlock& block) {
        if (expected.load() >= 0 && block.inputPosition != expected.load())
            continuous = false;
        expected = block.inputPosition + block.frames;
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count();
        if (now - lastHold.load() >= 2000)
        {
            lastHold = now;
            ++holds;
            std::this_thread::sleep_for(50ms);
        }
    };

    AudioError error;
    AudioStreamRequest request;
    request.inputChannels = {0, 1};
    request.inputBufferSeconds = 0.2;
    auto stream = devices.open("Windows Audio", input->id, request, error);
    REQUIRE(stream != nullptr);
    REQUIRE(!stream->add(recording));
    const auto started = stream->start();
    INFO(started.message << " (" << started.step << ", " << started.code << ")");
    REQUIRE(!started);
    std::this_thread::sleep_for(10s);
    const auto info = stream->info();
    const auto statistics = stream->statistics();
    REQUIRE(!stream->stop());

    INFO(input->name << ": rate " << info.sampleRate << ", block " << info.maxBlock << ", " << recording.frames
                     << " frames, " << holds << " holds, " << statistics.dropouts << " dropouts, priority "
                     << info.priority);
    CHECK(holds >= 4);
    CHECK(statistics.dropouts == 0);
    CHECK(continuous);
    CHECK(std::abs(static_cast<double>(recording.frames) - 10.0 * info.sampleRate) < 0.05 * info.sampleRate);
}

TEST_CASE("An input held up for 400 ms loses about what its buffer can't hold, and says where")
{
    const auto part = setting("SYMBOLENGINE_TEST_INPUT");
    if (part.empty())
        SKIP("SYMBOLENGINE_TEST_INPUT names the input to record from");

    Events events;
    AudioDevices devices(options(events));
    const auto list = devices.devices("Windows Audio");
    const auto* input = named(list, part, true);
    REQUIRE(input != nullptr);

    Recording recording;
    std::atomic<int> holds{0};
    recording.onProcess = [&](const AudioBlock& block) {
        if (block.inputPosition > 48000 && holds.load() == 0)
        {
            ++holds;
            std::this_thread::sleep_for(400ms);
        }
    };

    AudioError error;
    AudioStreamRequest request;
    request.inputChannels = {0, 1};
    request.inputBufferSeconds = 0.2;
    auto stream = devices.open("Windows Audio", input->id, request, error);
    REQUIRE(stream != nullptr);
    REQUIRE(!stream->add(recording));
    REQUIRE(!stream->start());
    std::this_thread::sleep_for(3s);
    const auto info = stream->info();
    const auto statistics = stream->statistics();
    REQUIRE(!stream->stop());
    REQUIRE(events.waitFor(AudioEvent::Kind::dropout));

    const auto lost = static_cast<double>(statistics.lostFrames) / info.sampleRate;
    INFO(statistics.dropouts << " dropouts, " << lost << " s lost");
    CHECK(holds == 1);
    CHECK(statistics.dropouts >= 1);
    CHECK(lost > 0.1);
    CHECK(lost < 0.35);
}

TEST_CASE("Exclusive mode plays silence through the output named for two seconds, at a format it takes, nothing lost")
{
    const auto part = setting("SYMBOLENGINE_TEST_OUTPUT");
    if (part.empty())
        SKIP("SYMBOLENGINE_TEST_OUTPUT names the output to play silence through");

    Events events;
    AudioDevices devices(options(events));
    const auto list = devices.devices("Windows Audio");
    const auto* output = named(list, part, false);
    REQUIRE(output != nullptr);

    Recording recording;
    AudioError error;
    AudioStreamRequest request;
    request.outputChannels = {0, 1};
    request.exclusive = true;
    auto stream = devices.open("Windows Audio", output->id, request, error);
    REQUIRE(stream != nullptr);
    REQUIRE(!stream->add(recording));
    const auto started = stream->start();
    INFO(started.message << " (" << started.step << ", " << started.code << ")");
    REQUIRE(!started);
    std::this_thread::sleep_for(2s);
    const auto info = stream->info();
    REQUIRE(!stream->stop());

    INFO(output->name << " in exclusive mode: rate " << info.sampleRate << ", block " << info.maxBlock << ", "
                      << recording.frames << " frames, output latency " << info.outputLatency << ", priority "
                      << info.priority);
    CHECK(info.sampleRate > 0.0);
    CHECK(recording.largest <= info.maxBlock);
    CHECK(recording.emptyBlocks == 0);
    CHECK(events.count(AudioEvent::Kind::stalled) == 0);
    CHECK(events.count(AudioEvent::Kind::dropout) == 0);
    CHECK(std::abs(static_cast<double>(recording.frames) - 2.0 * info.sampleRate) < 0.15 * info.sampleRate);
    CHECK(info.priority.find("Pro Audio") != std::string::npos);
}

TEST_CASE("Exclusive mode records two seconds from the input named, continuous and live, and shared mode asks for the raw stream")
{
    const auto part = setting("SYMBOLENGINE_TEST_INPUT");
    if (part.empty())
        SKIP("SYMBOLENGINE_TEST_INPUT names the input to record from");

    for (const bool exclusive : {true, false})
    {
        INFO((exclusive ? "exclusive" : "shared") << " mode");
        Events events;
        AudioDevices devices(options(events));
        const auto list = devices.devices("Windows Audio");
        const auto* input = named(list, part, true);
        REQUIRE(input != nullptr);

        Recording recording;
        std::atomic<std::int64_t> expected{-1};
        std::atomic<bool> continuous{true};
        std::atomic<float> peak{0.0f};
        recording.onProcess = [&](const AudioBlock& block) {
            if (expected.load() >= 0 && block.inputPosition != expected.load())
                continuous = false;
            expected = block.inputPosition + block.frames;
            auto loudest = peak.load();
            for (int c = 0; c < block.inputChannels; ++c)
                for (int i = 0; i < block.frames; ++i)
                    loudest = std::max(loudest, std::abs(block.inputs[c][i]));
            peak = loudest;
        };

        AudioError error;
        AudioStreamRequest request;
        request.inputChannels = {0, 1};
        request.exclusive = exclusive;
        auto stream = devices.open("Windows Audio", input->id, request, error);
        REQUIRE(stream != nullptr);
        REQUIRE(!stream->add(recording));
        const auto started = stream->start();
        INFO(started.message << " (" << started.step << ", " << started.code << ")");
        REQUIRE(!started);
        std::this_thread::sleep_for(2s);
        const auto info = stream->info();
        REQUIRE(!stream->stop());

        INFO(input->name << ": rate " << info.sampleRate << ", block " << info.maxBlock << ", " << recording.frames
                         << " frames, peak " << 20.0 * std::log10(std::max(peak.load(), 1e-12f)) << " dBFS, input latency "
                         << info.inputLatency << ", without the system's effects " << info.unprocessed << ", priority "
                         << info.priority);
        CHECK(info.sampleRate > 0.0);
        CHECK(continuous);
        CHECK(events.count(AudioEvent::Kind::dropout) == 0);
        CHECK(events.count(AudioEvent::Kind::stalled) == 0);
        CHECK(peak.load() > 0.0f);
        CHECK(std::abs(static_cast<double>(recording.frames) - 2.0 * info.sampleRate) < 0.15 * info.sampleRate);
        if (!exclusive)
        {
            const auto offered = offersRaw(input->id);
            INFO("Windows says the input offers the raw stream: " << offered);
            CHECK(info.unprocessed == offered);
        }
    }
}

// Run by the test below in a process of its own, as another program: the input named, held in exclusive mode.
TEST_CASE("Holds the input named for its own for three seconds, for another test", "[.]")
{
    Events events;
    AudioDevices devices(options(events));
    const auto list = devices.devices("Windows Audio");
    const auto* input = named(list, setting("SYMBOLENGINE_TEST_INPUT"), true);
    REQUIRE(input != nullptr);
    AudioError error;
    AudioStreamRequest request;
    request.inputChannels = {0, 1};
    request.exclusive = true;
    auto stream = devices.open("Windows Audio", input->id, request, error);
    REQUIRE(stream != nullptr);
    REQUIRE(!stream->start());
    std::this_thread::sleep_for(3s);
    REQUIRE(!stream->stop());
}

TEST_CASE("Another program taking the input for its own ends a shared stream on it, saying so")
{
    const auto part = setting("SYMBOLENGINE_TEST_INPUT");
    if (part.empty())
        SKIP("SYMBOLENGINE_TEST_INPUT names the input to record from");

    Events events;
    AudioDevices devices(options(events));
    const auto list = devices.devices("Windows Audio");
    const auto* input = named(list, part, true);
    REQUIRE(input != nullptr);
    Recording recording;
    AudioError error;
    AudioStreamRequest request;
    request.inputChannels = {0, 1};
    auto stream = devices.open("Windows Audio", input->id, request, error);
    REQUIRE(stream != nullptr);
    REQUIRE(!stream->add(recording));
    REQUIRE(!stream->start());
    REQUIRE(soon([&] { return recording.frames > 4800; }));

    // This program again, as another, taking the input for its own.
    wchar_t self[MAX_PATH]{};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring command = L"\"" + std::wstring(self) + L"\" \"Holds the input named for its own*\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION other{};
    REQUIRE(CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                           &startup, &other));

    const bool lost = events.waitFor(AudioEvent::Kind::lost, 1, 8000ms);
    WaitForSingleObject(other.hProcess, 10000);
    DWORD code = 1;
    GetExitCodeProcess(other.hProcess, &code);
    CloseHandle(other.hThread);
    CloseHandle(other.hProcess);

    CHECK(code == 0);
    REQUIRE(lost);
    const auto event = events.of(AudioEvent::Kind::lost).front();
    INFO(event.error.message);
    CHECK(event.loss == AudioLoss::takenExclusively);
    CHECK(stream->state() == AudioStream::State::lost);
}
