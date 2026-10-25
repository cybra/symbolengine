// The devices section's lessons for the lists: identity, listing without opening, devices that don't answer, one
// stream to a device, and the defaults' noisy notices.

#include "Helpers.h"

#include <symbolengine/devices/AudioDevices.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

using namespace symbolengine;
using Kind = AudioEvent::Kind;

namespace
{
AudioDevices::Options optionsFor(std::shared_ptr<FakeAudioSystem> system, Events& events)
{
    AudioDevices::Options options;
    options.systems = {std::move(system)};
    options.onEvent = events.handler();
    options.describeTimeout = 300ms;
    options.settle = 20ms;
    options.streams = quick(events);
    return options;
}

const AudioDeviceInfo* named(const std::vector<AudioDeviceInfo>& devices, const std::string& id)
{
    const auto found = std::find_if(devices.begin(), devices.end(), [&](const auto& d) { return d.id == id; });
    return found != devices.end() ? &*found : nullptr;
}
}

TEST_CASE("Listing devices and asking what they offer opens nothing and sets nothing")
{
    auto system = std::make_shared<FakeAudioSystem>();
    system->add(fakeDevice("a"));
    system->add(fakeDevice("b", 0, 2));
    Events events;
    AudioDevices devices(optionsFor(system, events));

    const auto list = devices.devices("Fake");
    CHECK(list.size() == 2);
    CHECK(devices.systems() == std::vector<std::string>{"Fake"});
    system->add(fakeDevice("c"));
    REQUIRE(events.waitFor(Kind::devicesChanged));
    CHECK(soon([&] { return devices.devices("Fake").size() == 3; }));

    CHECK(system->opens() == 0);
    CHECK(system->sets() == 0);
}

TEST_CASE("Devices with the same name are numbered among themselves")
{
    auto system = std::make_shared<FakeAudioSystem>();
    system->add(fakeDevice("unit-1", 2, 2, "Interface"));
    system->add(fakeDevice("built-in", 1, 2, "Speakers"));
    system->add(fakeDevice("unit-2", 2, 2, "Interface"));
    Events events;
    AudioDevices devices(optionsFor(system, events));

    const auto list = devices.devices("Fake");
    REQUIRE(named(list, "unit-1") != nullptr);
    CHECK(named(list, "unit-1")->sameNameIndex == 0);
    CHECK(named(list, "unit-2")->sameNameIndex == 1);
    CHECK(named(list, "built-in")->sameNameIndex == 0);
}

TEST_CASE("A device failing to answer is listed as failed and the rest are listed")
{
    auto system = std::make_shared<FakeAudioSystem>();
    system->add(fakeDevice("one"));
    auto& second = system->add(fakeDevice("two"));
    system->add(fakeDevice("three"));
    system->add(fakeDevice("four"));
    second.failDescribe = true;
    Events events;
    AudioDevices devices(optionsFor(system, events));

    const auto list = devices.devices("Fake");
    REQUIRE(list.size() == 4);
    CHECK_FALSE(named(list, "two")->answering);
    CHECK_FALSE(named(list, "two")->problem.empty());
    for (const auto* id : {"one", "three", "four"})
        CHECK(named(list, id)->answering);
}

TEST_CASE("A device whose calls never return is marked as not answering, and listing finishes")
{
    auto system = std::make_shared<FakeAudioSystem>();
    system->add(fakeDevice("fine"));
    auto& stuck = system->add(fakeDevice("stuck"));
    stuck.hangDescribe = true;
    Events events;

    const auto before = std::chrono::steady_clock::now();
    {
        AudioDevices devices(optionsFor(system, events));
        CHECK(std::chrono::steady_clock::now() - before < 2s);

        const auto list = devices.devices("Fake");
        REQUIRE(list.size() == 2);
        CHECK(named(list, "fine")->answering);
        CHECK_FALSE(named(list, "stuck")->answering);
    }
    stuck.hangDescribe = false;
}

TEST_CASE("A second stream on one device is refused")
{
    auto system = std::make_shared<FakeAudioSystem>();
    system->add(fakeDevice("interface"));
    Events events;
    AudioDevices devices(optionsFor(system, events));

    AudioError error;
    auto first = devices.open("Fake", "interface", stereo(), error);
    REQUIRE(first != nullptr);
    CHECK_FALSE(error);
    auto second = devices.open("Fake", "interface", stereo(), error);
    CHECK(second == nullptr);
    CHECK(error);

    first.reset();
    error = {};
    auto third = devices.open("Fake", "interface", stereo(), error);
    CHECK(third != nullptr);
}

TEST_CASE("A burst of default-changed notices for every role and direction makes one change")
{
    auto system = std::make_shared<FakeAudioSystem>();
    system->add(fakeDevice("speakers", 0, 2));
    system->add(fakeDevice("headphones", 0, 2));
    system->setDefault(AudioDirection::output, "speakers");
    Events events;
    AudioDevices devices(optionsFor(system, events));
    CHECK(devices.defaultDevice("Fake", AudioDirection::output) == "speakers");

    system->setDefault(AudioDirection::output, "headphones", false);
    system->notify(AudioSystemChange::defaults, 6, 3);
    REQUIRE(events.waitFor(Kind::defaultChanged));
    std::this_thread::sleep_for(100ms);

    CHECK(events.count(Kind::defaultChanged) == 1);
    const auto change = events.of(Kind::defaultChanged).front();
    CHECK(change.direction == AudioDirection::output);
    CHECK(change.deviceId == "headphones");
    CHECK(devices.defaultDevice("Fake", AudioDirection::output) == "headphones");

    // A notice with nothing changed says nothing.
    system->notify(AudioSystemChange::defaults, 3);
    std::this_thread::sleep_for(100ms);
    CHECK(events.count(Kind::defaultChanged) == 1);
}

TEST_CASE("An input stream lost leaves an output stream on another device running")
{
    auto system = std::make_shared<FakeAudioSystem>();
    auto& microphone = system->add(fakeDevice("microphone", 1, 0));
    auto& speakers = system->add(fakeDevice("speakers", 0, 2));
    Events events;
    AudioDevices devices(optionsFor(system, events));

    AudioError error;
    AudioStreamRequest in, out;
    in.inputChannels = {0};
    out.outputChannels = {0, 1};
    auto input = devices.open("Fake", "microphone", in, error);
    auto output = devices.open("Fake", "speakers", out, error);
    REQUIRE(input != nullptr);
    REQUIRE(output != nullptr);
    REQUIRE(!input->start());
    REQUIRE(!output->start());
    REQUIRE(microphone.cycle());
    REQUIRE(speakers.cycle());

    system->remove("microphone");
    REQUIRE(events.waitFor(Kind::lost));
    std::this_thread::sleep_for(50ms);
    CHECK(events.count(Kind::lost) == 1);
    CHECK(events.of(Kind::lost).front().deviceId == "microphone");
    CHECK(output->state() == AudioStream::State::running);
    CHECK(speakers.cycle());
}

TEST_CASE("Resolving a setup uses the devices as listed")
{
    auto system = std::make_shared<FakeAudioSystem>();
    system->add(fakeDevice("interface"));
    system->setDefault(AudioDirection::output, "interface");
    Events events;
    AudioDevices devices(optionsFor(system, events));

    AudioSetup setup;
    setup.system = "Fake";
    const auto resolved = devices.resolve(setup);
    CHECK(resolved.output.id == "interface");
    CHECK(resolved.output.exact);
    CHECK(resolved.input.id.empty());
}

TEST_CASE("A stream following the default takes its new device with it, which another can't open and its old one can")
{
    auto system = std::make_shared<FakeAudioSystem>();
    system->add(fakeDevice("speakers", 0, 2));
    system->add(fakeDevice("headphones", 0, 2));
    system->setDefault(AudioDirection::output, "speakers");
    Events events;
    AudioDevices devices(optionsFor(system, events));

    AudioError error;
    auto request = stereo(false, true);
    request.followDefault = true;
    auto following = devices.open("Fake", "speakers", request, error);
    REQUIRE(following != nullptr);
    REQUIRE(!following->start());
    system->setDefault(AudioDirection::output, "headphones");
    REQUIRE(events.waitFor(Kind::moved));

    CHECK(devices.open("Fake", "headphones", stereo(false, true), error) == nullptr);
    CHECK(error.message == "The audio device is already open.");
    CHECK(devices.open("Fake", "speakers", stereo(false, true), error) != nullptr);
    following.reset();
    CHECK(devices.open("Fake", "headphones", stereo(false, true), error) != nullptr);
}
