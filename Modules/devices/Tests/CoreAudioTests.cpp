// Core Audio's lessons. The arithmetic is checked on any Mac; with SYMBOLENGINE_REAL_DEVICES=1 the tests also list
// the Mac's own devices, play silence through its built-in speakers, which runners have none of, and record from its
// own microphone, asking for the microphone first, or from the input SYMBOLENGINE_TEST_INPUT names part of: a
// MacBook's own microphone is cut off while its lid is closed, and gives exact silence. The input muted for this
// program, from macOS 14, is set by the test itself, and needs an input that is live.

#include "CoreAudioDetail.h"
#include "Helpers.h"

#include <symbolengine/core/System.h>
#include <symbolengine/devices/AudioDevices.h>
#include <symbolengine/devices/Microphone.h>

#include <catch2/catch_test_macros.hpp>

#include <CoreAudio/CoreAudio.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <future>

using namespace symbolengine;
namespace detail = symbolengine::coreAudio::detail;

namespace
{
bool realDevices()
{
    return system::environmentVariable("SYMBOLENGINE_REAL_DEVICES").value_or("") == "1";
}

const AudioDeviceInfo* builtInSpeakers(const std::vector<AudioDeviceInfo>& devices)
{
    for (const auto& device : devices)
        if (device.transport == AudioTransport::builtIn && device.outputs() > 0 && device.inputs() == 0)
            return &device;
    return nullptr;
}

const AudioDeviceInfo* testInput(const std::vector<AudioDeviceInfo>& devices)
{
    const auto part = system::environmentVariable("SYMBOLENGINE_TEST_INPUT").value_or("");
    for (const auto& device : devices)
        if (device.inputs() > 0 && (part.empty() ? device.transport == AudioTransport::builtIn && device.outputs() == 0
                                                 : device.name.find(part) != std::string::npos))
            return &device;
    return nullptr;
}
}

TEST_CASE("Core Audio's host times in ticks come out in nanoseconds at Apple Silicon's timebase and Intel's")
{
    // 125/3: 24 million ticks a second.
    CHECK(detail::nanosecondsFromTicks(24'000'000, 125, 3) == 1'000'000'000);
    CHECK(detail::nanosecondsFromTicks(3, 125, 3) == 125);
    CHECK(detail::nanosecondsFromTicks(1'000'000'000, 1, 1) == 1'000'000'000);
    // A day of ticks, which multiplying first would overflow past.
    const std::uint64_t day = 24ull * 3600 * 24'000'000;
    CHECK(detail::nanosecondsFromTicks(day, 125, 3) == 24ll * 3600 * 1'000'000'000);
}

TEST_CASE("Core Audio's rate ranges give each one rate as it is and the standard rates within a wider one")
{
    const auto rates = detail::ratesWithin({{44100, 44100}, {48000, 48000}, {88200, 192000}}, 48000);
    CHECK(rates == std::vector<double>{44100, 48000, 88200, 96000, 128000, 176400, 192000});

    // AirPods at 24 kHz alone, a rate no fixed list would try.
    CHECK(detail::ratesWithin({{24000, 24000}}, 24000) == std::vector<double>{24000});

    // A range off by a hertz still holds the rate.
    CHECK(detail::ratesWithin({{44099.998, 48000.5}}, 0) == std::vector<double>{44100, 48000});
}

TEST_CASE("Core Audio's block size range gives its powers of two and the size it runs at")
{
    CHECK(detail::blockSizesWithin(15, 480, 480) == std::vector<int>{16, 32, 64, 128, 256, 480});
    CHECK(detail::blockSizesWithin(32, 4096, 512) == std::vector<int>{32, 64, 128, 256, 512, 1024, 2048, 4096});
}

TEST_CASE("Core Audio's channels are found in each stream, interleaved or not")
{
    // A stereo stream, a mono one and a six-channel one: channels 0 and 1, 2, then 3 to 8.
    const auto places = detail::placesOf({0, 2, 5, 8, 9}, {2, 1, 6});
    REQUIRE(places.size() == 5);
    CHECK((places[0].buffer == 0 && places[0].channel == 0 && places[0].channels == 2));
    CHECK((places[1].buffer == 1 && places[1].channel == 0 && places[1].channels == 1));
    CHECK((places[2].buffer == 2 && places[2].channel == 2 && places[2].channels == 6));
    CHECK((places[3].buffer == 2 && places[3].channel == 5));
    CHECK(places[4].buffer == -1);
}

TEST_CASE("This Mac's devices are listed without opening any, each with its channels, rates and block sizes")
{
    if (!realDevices())
        SKIP("SYMBOLENGINE_REAL_DEVICES=1 lists the Mac's own devices");

    Events events;
    AudioDevices devices({.onEvent = events.handler()});
    REQUIRE(devices.systems() == std::vector<std::string>{"Core Audio"});
    const auto list = devices.devices("Core Audio");
    REQUIRE_FALSE(list.empty());
    for (const auto& device : list)
    {
        INFO(device.name);
        CHECK_FALSE(device.id.empty());
        CHECK(device.answering);
        CHECK(device.inputs() + device.outputs() > 0);
        CHECK(device.currentRate > 0.0);
        CHECK(std::find(device.sampleRates.begin(), device.sampleRates.end(), device.currentRate) !=
              device.sampleRates.end());
        CHECK(device.currentBlock > 0);
    }

    // The same devices again, by the same identifiers.
    const auto again = devices.devices("Core Audio");
    REQUIRE(again.size() == list.size());
    for (std::size_t i = 0; i < list.size(); ++i)
        CHECK(again[i].id == list[i].id);
    CHECK(builtInSpeakers(list) != nullptr);
    CHECK_FALSE(devices.defaultDevice("Core Audio", AudioDirection::output).empty());
}

TEST_CASE("Silence played through the built-in speakers for two seconds comes in blocks at the device's own rate")
{
    if (!realDevices())
        SKIP("SYMBOLENGINE_REAL_DEVICES=1 plays silence through the Mac's built-in speakers");

    Events events;
    AudioDevices devices({.onEvent = events.handler()});
    const auto list = devices.devices("Core Audio");
    const auto* speakers = builtInSpeakers(list);
    REQUIRE(speakers != nullptr);

    Recording recording;
    std::atomic<std::int64_t> firstTime{-1}, lastTime{-1};
    std::atomic<bool> continuous{true};
    std::atomic<std::int64_t> expected{0};
    recording.onProcess = [&](const AudioBlock& block) {
        if (firstTime.load() < 0)
            firstTime = block.outputTime;
        lastTime = block.outputTime;
        if (block.outputPosition != expected.load())
            continuous = false;
        expected = block.outputPosition + block.frames;
        for (int c = 0; c < block.outputChannels; ++c)
            std::fill(block.outputs[c], block.outputs[c] + block.frames, 0.0f);
    };

    AudioError error;
    AudioStreamRequest request;
    request.outputChannels = {0, 1};
    auto stream = devices.open("Core Audio", speakers->id, request, error);
    REQUIRE(stream != nullptr);
    REQUIRE(!stream->add(recording));
    REQUIRE(!stream->start());
    std::this_thread::sleep_for(2s);
    const auto info = stream->info();
    REQUIRE(!stream->stop());

    INFO("rate " << info.sampleRate << ", block " << info.maxBlock << ", " << recording.frames << " frames");
    CHECK(info.sampleRate == speakers->currentRate);
    CHECK(recording.largest <= info.maxBlock);
    CHECK(recording.emptyBlocks == 0);
    CHECK(continuous);
    CHECK(stream->statistics().dropouts == 0);
    CHECK(events.count(AudioEvent::Kind::stalled) == 0);

    // Its frames over its host times run at its rate, within half a percent.
    const auto seconds = static_cast<double>(lastTime - firstTime) / 1e9;
    REQUIRE(seconds > 1.5);
    const auto measured = static_cast<double>(recording.frames - recording.kept().back().frames) / seconds;
    CHECK(std::abs(measured - info.sampleRate) / info.sampleRate < 0.005);
    CHECK(info.outputLatency > info.maxBlock);
}

TEST_CASE("Two seconds from the Mac's own microphone come in at its rate and live, once the program may use it")
{
    if (!realDevices())
        SKIP("SYMBOLENGINE_REAL_DEVICES=1 records from the Mac's own microphone");

    // Asked first, as a program asks when recording is chosen; the user answers the system's question.
    if (microphoneAccess() == MicrophoneAccess::notAsked)
    {
        std::promise<bool> answer;
        askForMicrophone([&answer](bool allowed) { answer.set_value(allowed); });
        REQUIRE(answer.get_future().wait_for(60s) == std::future_status::ready);
    }

    Events events;
    AudioDevices devices({.onEvent = events.handler()});
    const auto list = devices.devices("Core Audio");
    const auto* input = testInput(list);
    REQUIRE(input != nullptr);

    AudioError error;
    AudioStreamRequest request;
    request.inputChannels = {0};
    auto stream = devices.open("Core Audio", input->id, request, error);
    REQUIRE(stream != nullptr);

    // Not allowed, the input says so when it starts, where Core Audio would give silence.
    if (microphoneAccess() == MicrophoneAccess::denied)
    {
        const auto refused = stream->start();
        CHECK(refused.step == "microphone permission");
        SKIP("This terminal may not use the microphone: " << refused.message);
    }

    Recording recording;
    std::atomic<std::int64_t> firstTime{-1}, lastTime{-1}, expected{0}, zeros{0}, samples{0};
    std::atomic<bool> continuous{true};
    std::atomic<float> peak{0.0f};
    recording.onProcess = [&](const AudioBlock& block) {
        if (firstTime.load() < 0)
            firstTime = block.inputTime;
        lastTime = block.inputTime;
        if (block.inputPosition != expected.load())
            continuous = false;
        expected = block.inputPosition + block.frames;
        auto loudest = peak.load();
        for (int i = 0; i < block.frames; ++i)
        {
            const auto value = block.inputs[0][i];
            loudest = std::max(loudest, std::abs(value));
            if (value == 0.0f)
                ++zeros;
        }
        peak = loudest;
        samples += block.frames;
    };

    REQUIRE(!stream->add(recording));
    REQUIRE(!stream->start());
    std::this_thread::sleep_for(2s);
    const auto info = stream->info();
    REQUIRE(!stream->stop());

    INFO(input->name << ": rate " << info.sampleRate << ", block " << info.maxBlock << ", " << recording.frames
                     << " frames, peak " << 20.0 * std::log10(std::max(peak.load(), 1e-12f)) << " dBFS, " << zeros
                     << " samples exactly 0");
    CHECK(info.sampleRate == input->currentRate);
    CHECK(recording.emptyBlocks == 0);
    CHECK(continuous);
    CHECK(stream->statistics().dropouts == 0);
    CHECK(events.count(AudioEvent::Kind::stalled) == 0);

    // A live microphone's noise, never exact silence throughout.
    CHECK(peak.load() > 0.0f);
    CHECK(zeros.load() < samples.load() / 2);

    // Its frames over its host times run at its rate, within half a percent.
    const auto seconds = static_cast<double>(lastTime - firstTime) / 1e9;
    REQUIRE(seconds > 1.5);
    const auto measured = static_cast<double>(recording.frames - recording.kept().back().frames) / seconds;
    CHECK(std::abs(measured - info.sampleRate) / info.sampleRate < 0.005);
    CHECK(info.inputLatency > 0);
}

TEST_CASE("This program's input muted through Core Audio is said and gives silence, and unmuted gives sound again")
{
    if (!realDevices())
        SKIP("SYMBOLENGINE_REAL_DEVICES=1 records from the Mac's own input");
    const AudioObjectPropertyAddress mute{kAudioHardwarePropertyProcessInputMute, kAudioObjectPropertyScopeGlobal,
                                          kAudioObjectPropertyElementMain};
    if (!AudioObjectHasProperty(kAudioObjectSystemObject, &mute))
        SKIP("The program's own input mute came with macOS 14");
    if (microphoneAccess() != MicrophoneAccess::allowed)
        SKIP("This terminal may not use the microphone");

    // Unmuted again however the test ends.
    const auto setMute = [&](UInt32 value) {
        return AudioObjectSetPropertyData(kAudioObjectSystemObject, &mute, 0, nullptr, sizeof(value), &value);
    };
    struct Unmute
    {
        std::function<void()> undo;
        ~Unmute() { undo(); }
    } unmute{[&] { setMute(0); }};

    Events events;
    AudioDevices devices({.onEvent = events.handler()});
    const auto list = devices.devices("Core Audio");
    const auto* input = testInput(list);
    REQUIRE(input != nullptr);

    Recording recording;
    std::atomic<float> peak{0.0f};
    recording.onProcess = [&](const AudioBlock& block) {
        auto loudest = peak.load();
        for (int i = 0; i < block.frames; ++i)
            loudest = std::max(loudest, std::abs(block.inputs[0][i]));
        peak = loudest;
    };
    AudioError error;
    AudioStreamRequest request;
    request.inputChannels = {0};
    auto stream = devices.open("Core Audio", input->id, request, error);
    REQUIRE(stream != nullptr);
    REQUIRE(!stream->add(recording));
    REQUIRE(!stream->start());
    std::this_thread::sleep_for(500ms);
    INFO(input->name);
    REQUIRE(peak.load() > 0.0f);

    REQUIRE(setMute(1) == noErr);
    REQUIRE(events.waitFor(AudioEvent::Kind::inputMuted));
    std::this_thread::sleep_for(200ms);
    peak = 0.0f;
    std::this_thread::sleep_for(500ms);
    CHECK(peak.load() == 0.0f);

    REQUIRE(setMute(0) == noErr);
    REQUIRE(events.waitFor(AudioEvent::Kind::inputUnmuted));
    std::this_thread::sleep_for(200ms);
    peak = 0.0f;
    std::this_thread::sleep_for(500ms);
    CHECK(peak.load() > 0.0f);
    REQUIRE(!stream->stop());
}

TEST_CASE("A thread of the program's joins the built-in speakers' audio workgroup while they play, and leaves it")
{
    if (!realDevices())
        SKIP("SYMBOLENGINE_REAL_DEVICES=1 plays silence through the Mac's built-in speakers");
    AudioDevices devices;
    const auto list = devices.devices("Core Audio");
    const auto* speakers = builtInSpeakers(list);
    REQUIRE(speakers != nullptr);
    AudioError error;
    auto stream = devices.open("Core Audio", speakers->id, stereo(false, true), error);
    REQUIRE(stream != nullptr);
    Recording recording;
    REQUIRE(!stream->add(recording));
    REQUIRE(!stream->start());

    const auto group = stream->workgroup();
    REQUIRE_FALSE(group.empty());
    CHECK(group.maxThreads() >= 1);
    const auto info = stream->info();
    bool joined = false;
    std::thread worker([&] {
        const auto membership = group.join(info.sampleRate, info.maxBlock);
        joined = membership.joined();
    });
    worker.join();
    CHECK(joined);
    REQUIRE(!stream->stop());
}
