// Core Audio's lessons. The arithmetic is checked on any Mac; with SYMBOLENGINE_REAL_DEVICES=1 the tests also list
// the Mac's own devices and play silence through its built-in speakers, which runners have none of.

#include "CoreAudioDetail.h"
#include "Helpers.h"

#include <symbolengine/core/System.h>
#include <symbolengine/devices/AudioDevices.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

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
