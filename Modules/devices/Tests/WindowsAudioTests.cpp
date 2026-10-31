// Windows Audio's lessons on a real PC. With SYMBOLENGINE_REAL_DEVICES=1 they list its endpoints; with
// SYMBOLENGINE_TEST_OUTPUT and SYMBOLENGINE_TEST_INPUT naming part of an endpoint's name they play silence through
// that output and record from that input, in shared mode and exclusive, and another copy of these tests takes the
// input for its own while a stream records from it. Runners have no endpoints, so these are run by hand.

#include "Helpers.h"
#include "Systems.h"

#include <symbolengine/core/System.h>
#include <symbolengine/devices/AudioDevices.h>

#include <catch2/catch_test_macros.hpp>

#ifndef NOMINMAX
    #define NOMINMAX
#endif
#include <windows.h>

#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>

// Whether an endpoint offers the raw stream, the key Windows 8.1 added, which the SDK's headers don't declare.
const PROPERTYKEY rawProcessingSupported{{0x8943b373, 0x388c, 0x4395, {0xb5, 0x57, 0xbc, 0x6d, 0xba, 0xff, 0xaf, 0xdb}}, 2};

// The bus that enumerated an endpoint's device, DEVPKEY_Device_EnumeratorName.
const PROPERTYKEY enumeratorName{{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 24};

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

// Every endpoint the PC has known that a Bluetooth headset's hands-free profile made, present or not, by its
// identifier and name, read from its properties apart from the module.
std::vector<std::pair<std::string, std::string>> handsFreeEndpoints()
{
    const auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::vector<std::pair<std::string, std::string>> found;
    const auto text = [](IPropertyStore* store, const PROPERTYKEY& key) {
        PROPVARIANT value;
        PropVariantInit(&value);
        std::string result;
        if (SUCCEEDED(store->GetValue(key, &value)) && value.vt == VT_LPWSTR)
            for (const auto* c = value.pwszVal; *c != 0; ++c)
                result.push_back(*c < 128 ? static_cast<char>(*c) : '?');
        PropVariantClear(&value);
        return result;
    };
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDeviceCollection* all = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                   reinterpret_cast<void**>(&enumerator))) &&
        SUCCEEDED(enumerator->EnumAudioEndpoints(eAll, DEVICE_STATEMASK_ALL, &all)))
    {
        UINT count = 0;
        all->GetCount(&count);
        for (UINT i = 0; i < count; ++i)
        {
            IMMDevice* device = nullptr;
            IPropertyStore* properties = nullptr;
            LPWSTR id = nullptr;
            if (SUCCEEDED(all->Item(i, &device)) && SUCCEEDED(device->OpenPropertyStore(STGM_READ, &properties)) &&
                SUCCEEDED(device->GetId(&id)) && text(properties, enumeratorName) == "BTHHFENUM")
            {
                std::string identifier;
                for (const auto* c = id; *c != 0; ++c)
                    identifier.push_back(static_cast<char>(*c));
                found.emplace_back(identifier, text(properties, PKEY_Device_FriendlyName));
            }
            if (id != nullptr)
                CoTaskMemFree(id);
            if (properties != nullptr)
                properties->Release();
            if (device != nullptr)
                device->Release();
        }
    }
    if (all != nullptr)
        all->Release();
    if (enumerator != nullptr)
        enumerator->Release();
    if (SUCCEEDED(com))
        CoUninitialize();
    return found;
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
    std::atomic<std::int64_t> firstTime{-1}, lastTime{-1};
    recording.onProcess = [&](const AudioBlock& block) {
        if (firstTime.load() < 0)
            firstTime = block.outputTime;
        lastTime = block.outputTime;
    };
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

    // Its blocks say when they will be heard, on the steady clock, moving with what plays.
    CHECK(firstTime > 0);
    CHECK(std::abs(static_cast<double>(lastTime - firstTime) / 1e9 - 2.0) < 0.3);

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

TEST_CASE("A thread of the program's joins the multimedia scheduler's Pro Audio class for a stream, and leaves it")
{
    if (setting("SYMBOLENGINE_REAL_DEVICES") != "1")
        SKIP("SYMBOLENGINE_REAL_DEVICES=1 lists the PC's own endpoints");
    Events events;
    AudioDevices devices(options(events));
    const auto output = devices.defaultDevice("Windows Audio", AudioDirection::output);
    REQUIRE_FALSE(output.empty());
    AudioError error;
    AudioStreamRequest request;
    request.outputChannels = {0, 1};
    auto stream = devices.open("Windows Audio", output, request, error);
    REQUIRE(stream != nullptr);
    Recording recording;
    REQUIRE(!stream->add(recording));
    REQUIRE(!stream->start());
    const auto group = stream->workgroup();
    REQUIRE_FALSE(group.empty());
    bool joined = false;
    std::thread worker([&] { joined = group.join(48000.0, 480).joined(); });
    worker.join();
    CHECK(joined);
    REQUIRE(!stream->stop());
}

TEST_CASE("Every Bluetooth hands-free endpoint the PC has known is described as one, whatever Windows names it")
{
    if (setting("SYMBOLENGINE_REAL_DEVICES") != "1")
        SKIP("SYMBOLENGINE_REAL_DEVICES=1 reads the PC's own endpoints");
    const auto endpoints = handsFreeEndpoints();
    if (endpoints.empty())
        SKIP("This PC has known no Bluetooth headset's hands-free endpoint");

    std::shared_ptr<AudioSystem> windows;
    for (auto& system : devices::detail::systemsOfThisComputer())
        if (system->name() == "Windows Audio")
            windows = system;
    REQUIRE(windows != nullptr);
    WARN(endpoints.size() << " hands-free endpoints");
    for (const auto& [id, name] : endpoints)
    {
        INFO(name);
        const auto info = windows->describe(id);
        CHECK(info.handsFree);
        CHECK(info.transport == AudioTransport::bluetooth);
    }
}
