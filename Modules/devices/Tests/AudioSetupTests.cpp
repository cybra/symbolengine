// The devices section's lessons for choosing and restoring: each direction apart, none staying none, falling back
// setting by setting, never onto a hands-free microphone unasked, and every substitution said.

#include "Helpers.h"

#include <symbolengine/core/Json.h>
#include <symbolengine/devices/AudioSetup.h>

#include <catch2/catch_test_macros.hpp>

using namespace symbolengine;

namespace
{
AudioDeviceInfo device(std::string id, int inputs, int outputs, std::string name = {})
{
    auto info = fakeDevice(std::move(id), inputs, outputs, std::move(name));
    info.sampleRates = {44100.0, 48000.0, 96000.0};
    info.currentRate = 48000.0;
    info.blockSizes = {64, 128, 256, 512, 1024};
    info.currentBlock = 256;
    return info;
}

std::vector<AudioDeviceInfo> studio()
{
    auto headset = device("headset-handsfree", 1, 1, "Headset");
    headset.handsFree = true;
    auto loopback = device("loopback", 2, 2, "Loopback");
    loopback.loopback = true;
    return {device("built-in-mic", 1, 0, "Microphone"), device("built-in-out", 0, 2, "Speakers"),
            device("interface", 2, 2, "Interface"), headset, loopback};
}
}

TEST_CASE("An input saved as none stays closed when restored")
{
    AudioSetup setup;
    setup.input.choice = AudioSetup::Choice::none;
    setup.output.choice = AudioSetup::Choice::systemDefault;
    const auto resolved = setup.resolve(studio(), "built-in-mic", "built-in-out");
    CHECK(resolved.input.id.empty());
    CHECK(resolved.output.id == "built-in-out");
    CHECK(resolved.output.exact);
}

TEST_CASE("The saved input gone leaves the output exactly as saved")
{
    AudioSetup setup;
    setup.choose(AudioDirection::input, device("usb-mic", 1, 0, "USB Microphone"));
    setup.choose(AudioDirection::output, device("interface", 2, 2, "Interface"), {0, 1});
    setup.sampleRate = 96000.0;
    setup.blockSize = 128;

    const auto resolved = setup.resolve(studio(), "built-in-mic", "built-in-out");
    CHECK(resolved.output.id == "interface");
    CHECK(resolved.output.exact);
    CHECK(resolved.output.channels == std::vector<int>{0, 1});
    CHECK(resolved.output.sampleRate == 96000.0);
    CHECK(resolved.output.blockSize == 128);
    CHECK(resolved.output.said.empty());

    CHECK(resolved.input.id == "built-in-mic");
    CHECK_FALSE(resolved.input.exact);
    CHECK_FALSE(resolved.input.said.empty());
}

TEST_CASE("A headset's hands-free microphone or a loopback device is never put in another's place unasked")
{
    AudioSetup setup;
    setup.choose(AudioDirection::input, device("usb-mic", 1, 0, "USB Microphone"));

    // The system's default is the headset; the interface is chosen in its place.
    auto resolved = setup.resolve(studio(), "headset-handsfree", "built-in-out");
    CHECK(resolved.input.id != "headset-handsfree");
    CHECK(resolved.input.id != "loopback");
    CHECK_FALSE(resolved.input.id.empty());

    // Chosen before, it may be.
    setup.choose(AudioDirection::input, studio()[3]);
    setup.choose(AudioDirection::input, device("usb-mic", 1, 0, "USB Microphone"));
    resolved = setup.resolve(studio(), "headset-handsfree", "built-in-out");
    CHECK(resolved.input.id == "headset-handsfree");
}

TEST_CASE("A device that returns is found again as chosen")
{
    AudioSetup setup;
    setup.choose(AudioDirection::output, device("interface", 2, 2, "Interface"));

    auto without = studio();
    std::erase_if(without, [](const auto& d) { return d.id == "interface"; });
    auto resolved = setup.resolve(without, "built-in-mic", "built-in-out");
    CHECK(resolved.output.id == "built-in-out");
    CHECK_FALSE(resolved.output.exact);

    resolved = setup.resolve(studio(), "built-in-mic", "built-in-out");
    CHECK(resolved.output.id == "interface");
    CHECK(resolved.output.exact);
}

TEST_CASE("A device whose identifier changed is found by a name only it has, and two of one name are asked between")
{
    AudioSetup setup;
    setup.choose(AudioDirection::output, device("old-id", 2, 2, "Interface"));
    auto resolved = setup.resolve(studio(), "", "built-in-out");
    CHECK(resolved.output.id == "interface");
    CHECK(resolved.output.byName);

    auto twins = studio();
    twins.push_back(device("interface-2", 2, 2, "Interface"));
    resolved = setup.resolve(twins, "", "built-in-out");
    CHECK(resolved.output.id.empty());
    CHECK(resolved.output.ambiguous.size() == 2);
}

TEST_CASE("A rate or block size the device doesn't offer is put in place by the stated rule and said")
{
    AudioSetup setup;
    setup.choose(AudioDirection::output, device("interface", 2, 2, "Interface"));

    setup.sampleRate = 88200.0;
    setup.blockSize = 200;
    auto resolved = setup.resolve(studio(), "", "");
    CHECK(resolved.output.sampleRate == 96000.0);
    CHECK(resolved.output.blockSize == 256);
    CHECK_FALSE(resolved.output.said.empty());

    // As near to two, the higher; past the largest, the largest.
    setup.sampleRate = 46050.0;
    setup.blockSize = 4096;
    resolved = setup.resolve(studio(), "", "");
    CHECK(resolved.output.sampleRate == 48000.0);
    CHECK(resolved.output.blockSize == 1024);

    // Nothing saved: what the device runs at.
    setup.sampleRate = 0.0;
    setup.blockSize = 0;
    resolved = setup.resolve(studio(), "", "");
    CHECK(resolved.output.sampleRate == 0.0);
    CHECK(resolved.output.blockSize == 0);
    CHECK(resolved.output.said.empty());
}

TEST_CASE("A device that reads its rate as 0 is given one it offers")
{
    auto devices = studio();
    devices[2].currentRate = 0.0;
    AudioSetup setup;
    setup.choose(AudioDirection::output, devices[2]);
    const auto resolved = setup.resolve(devices, "", "");
    CHECK(resolved.output.sampleRate == 48000.0);
}

TEST_CASE("Saved channels a device no longer has are said, never recorded as fewer without a word")
{
    AudioSetup setup;
    setup.choose(AudioDirection::input, device("interface", 8, 8, "Interface"), {6, 7});
    const auto resolved = setup.resolve(studio(), "", "");
    CHECK(resolved.input.id == "interface");
    CHECK(resolved.input.channelsMissing);
    CHECK(resolved.input.channels.empty());
    CHECK_FALSE(resolved.input.said.empty());
}

TEST_CASE("No channels saved takes an input's first and an output's first two")
{
    AudioSetup setup;
    setup.choose(AudioDirection::input, device("interface", 2, 2, "Interface"));
    setup.choose(AudioDirection::output, device("interface", 2, 2, "Interface"));
    const auto resolved = setup.resolve(studio(), "", "");
    CHECK(resolved.input.channels == std::vector<int>{0});
    CHECK(resolved.output.channels == std::vector<int>{0, 1});
}

TEST_CASE("An ASIO driver not yet read is taken at its word, and never put in another's place")
{
    AudioDeviceInfo unread;
    unread.id = "{driver}";
    unread.name = "Interface USB ASIO";
    unread.unread = true;
    const std::vector<AudioDeviceInfo> drivers{unread};

    // Chosen with channels and settings saved: all of them, as saved, until the driver says what it has.
    AudioSetup setup;
    setup.system = "ASIO";
    setup.choose(AudioDirection::input, device("{driver}", 8, 8, "Interface USB ASIO"), {6, 7});
    setup.choose(AudioDirection::output, device("{driver}", 8, 8, "Interface USB ASIO"), {2, 3});
    setup.sampleRate = 96000.0;
    setup.blockSize = 64;
    auto resolved = setup.resolve(drivers, "", "");
    CHECK(resolved.input.id == "{driver}");
    CHECK(resolved.input.channels == std::vector<int>{6, 7});
    CHECK_FALSE(resolved.input.channelsMissing);
    CHECK(resolved.output.channels == std::vector<int>{2, 3});
    CHECK(resolved.output.sampleRate == 96000.0);
    CHECK(resolved.output.blockSize == 64);
    CHECK(resolved.output.said.empty());

    // Found by its name when its identifier changed, and given an input's first and an output's first two.
    AudioSetup renamed;
    renamed.output.choice = AudioSetup::Choice::device;
    renamed.output.id = "{old}";
    renamed.output.name = "Interface USB ASIO";
    resolved = renamed.resolve(drivers, "", "");
    CHECK(resolved.output.id == "{driver}");
    CHECK(resolved.output.byName);
    CHECK(resolved.output.channels == std::vector<int>{0, 1});

    // A driver another stood for isn't loaded in its place.
    AudioSetup missing;
    missing.output.choice = AudioSetup::Choice::device;
    missing.output.id = "{gone}";
    missing.output.name = "Another ASIO";
    CHECK(missing.resolve(drivers, "", "").output.id.empty());
}

TEST_CASE("A setup reads back as it was written, and anything missing or wrong reads as its default")
{
    AudioSetup setup;
    setup.system = "Core Audio";
    setup.choose(AudioDirection::input, device("interface", 2, 2, "Interface"), {1});
    setup.output.choice = AudioSetup::Choice::systemDefault;
    setup.sampleRate = 48000.0;
    setup.blockSize = 256;
    setup.exclusive = true;

    const auto back = AudioSetup::fromValue(json::parse(json::write(setup.toValue())));
    CHECK(back.system == "Core Audio");
    CHECK(back.input.choice == AudioSetup::Choice::device);
    CHECK(back.input.id == "interface");
    CHECK(back.input.name == "Interface");
    CHECK(back.input.channels == std::vector<int>{1});
    CHECK(back.input.chosenBefore == std::vector<std::string>{"interface"});
    CHECK(back.output.choice == AudioSetup::Choice::systemDefault);
    CHECK(back.sampleRate == 48000.0);
    CHECK(back.blockSize == 256);
    CHECK(back.exclusive);

    const auto garbled = AudioSetup::fromValue(json::parse(R"({"input": 3, "output": {"choice": "sideways"},
                                                                "sampleRate": "fast"})"));
    CHECK(garbled.input.choice == AudioSetup::Choice::none);
    CHECK(garbled.output.choice == AudioSetup::Choice::systemDefault);
    CHECK(garbled.sampleRate == 0.0);
}
