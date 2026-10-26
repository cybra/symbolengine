// What happens to devices recorded and played back: a session on one system written down line by line, and a fake
// system given those lines meeting a stream with what the first one met, as a test meets real hardware's notices.

#include "AudioRecordFormat.h"
#include "Helpers.h"

#include <symbolengine/core/Json.h>
#include <symbolengine/devices/AudioDevices.h>
#include <symbolengine/devices/RecordingAudioSystem.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>

using namespace symbolengine;
using Kind = AudioEvent::Kind;

namespace
{
class Lines
{
public:
    std::function<void(const std::string&)> writer()
    {
        return [this](const std::string& line) {
            const std::lock_guard hold(lock);
            kept.push_back(line);
        };
    }

    std::vector<std::string> all() const
    {
        const std::lock_guard hold(lock);
        return kept;
    }

    std::vector<std::string> events() const
    {
        std::vector<std::string> found;
        for (const auto& line : all())
            found.push_back(json::parse(line)["event"].stringOr(""));
        return found;
    }

    bool has(const std::string& event) const
    {
        const auto found = events();
        return std::find(found.begin(), found.end(), event) != found.end();
    }

private:
    mutable std::mutex lock;
    std::vector<std::string> kept;
};

// A session recorded on real hardware with the owner, trimmed, its devices' identifiers and names replaced
// (Tests/Sessions), split where the recorder first opened a stream: the devices as they were then, and the rest.
struct Session
{
    std::vector<std::string> before, after;
};

Session session(const char* name)
{
    std::ifstream file(std::filesystem::path(SYMBOLENGINE_SESSIONS_FOLDER) / name);
    Session found;
    bool opened = false;
    for (std::string line; std::getline(file, line);)
    {
        if (line.empty())
            continue;
        opened = opened || json::parse(line)["event"].stringOr("") == "open";
        (opened ? found.after : found.before).push_back(line);
    }
    return found;
}

// The sleeps, wakings and losses a stream met, in order.
std::vector<Kind> met(const Events& events)
{
    std::vector<Kind> kinds;
    for (const auto& event : events.all())
        if (event.kind == Kind::sleeping || event.kind == Kind::woke || event.kind == Kind::lost)
            kinds.push_back(event.kind);
    return kinds;
}

AudioDevices::Options optionsFor(std::shared_ptr<AudioSystem> system, Events& events)
{
    AudioDevices::Options options;
    options.systems = {std::move(system)};
    options.onEvent = events.handler();
    options.describeTimeout = 300ms;
    options.settle = 20ms;
    options.streams = quick(events);
    return options;
}
}

TEST_CASE("A device's description reads back from its line as it was written")
{
    auto device = fakeDevice("headset", 1, 2, "Headset");
    device.transport = AudioTransport::bluetooth;
    device.handsFree = true;
    device.clockDomain = 7;
    device.sampleRates = {16000.0, 48000.0};
    device.blockSizes = {256, 512};
    device.currentRate = 16000.0;
    device.currentBlock = 256;
    device.problem = "Asleep";
    device.answering = false;
    const auto back = devices::detail::deviceFrom(json::parse(json::write(devices::detail::toValue(device))));
    CHECK(back.id == device.id);
    CHECK(back.name == device.name);
    CHECK(back.transport == AudioTransport::bluetooth);
    CHECK(back.inputNames == device.inputNames);
    CHECK(back.outputNames == device.outputNames);
    CHECK(back.sampleRates == device.sampleRates);
    CHECK(back.blockSizes == device.blockSizes);
    CHECK(back.currentRate == 16000.0);
    CHECK(back.currentBlock == 256);
    CHECK(back.handsFree);
    CHECK(back.clockDomain == 7);
    CHECK_FALSE(back.answering);
    CHECK(back.problem == "Asleep");
}

TEST_CASE("A stream whose start takes a while is recorded starting, with no stall while it starts", "[timing]")
{
    // As AirPods' hands-free input on Windows, which took 1.7 s to start while its call link connected.
    Lines lines;
    auto real = std::make_shared<FakeAudioSystem>();
    auto& headset = real->add(fakeDevice("headset", 2, 0, "Headset"));
    headset.run({.speed = 1.0});
    headset.startMilliseconds = 400;
    auto recorder = std::make_shared<RecordingAudioSystem>(real, lines.writer());
    Events events;
    AudioDevices devices(optionsFor(recorder, events));
    Recording recording(1 << 16);
    AudioError error;
    auto stream = devices.open("Fake", "headset", stereo(true, false), error);
    REQUIRE(stream != nullptr);
    REQUIRE(!stream->add(recording));
    REQUIRE(!stream->start());
    REQUIRE(soon([&] { return recording.frames > 4800; }));
    std::this_thread::sleep_for(200ms);
    CHECK(lines.has("start"));
    CHECK_FALSE(lines.has("stall"));
}

TEST_CASE("A session recorded on one system and played back on a fake meets a stream there with what it met first")
{
    // Recorded: an interface recorded from, its rate changed under it by another program, and then unplugged.
    Lines lines;
    {
        auto real = std::make_shared<FakeAudioSystem>();
        auto& interface = real->add(fakeDevice("interface", 2, 2, "Interface"));
        real->add(fakeDevice("speakers", 0, 2, "Speakers"));
        real->setDefault(AudioDirection::input, "interface");
        real->setDefault(AudioDirection::output, "speakers");
        interface.run({.speed = 1.0});
        auto recorder = std::make_shared<RecordingAudioSystem>(real, lines.writer());

        Events events;
        AudioDevices devices(optionsFor(recorder, events));
        Recording recording(1 << 16);
        AudioError error;
        auto stream = devices.open("Fake", "interface", stereo(true, false), error);
        REQUIRE(stream != nullptr);
        REQUIRE(!stream->add(recording));
        REQUIRE(!stream->start());
        REQUIRE(soon([&] { return recording.frames > 4800; }));

        interface.changed([](AudioDeviceInfo& info) { info.currentRate = 44100.0; });
        REQUIRE(events.waitFor(Kind::formatChanged));
        REQUIRE(soon([&] { return lines.has("changed"); }));
        std::this_thread::sleep_for(200ms);
        real->remove("interface");
        REQUIRE(events.waitFor(Kind::lost));
        REQUIRE(soon([&] { return lines.has("lost"); }));
        REQUIRE(soon([&] {
            const auto all = lines.all();
            const auto last = json::parse(all.back());
            return last["event"].stringOr("") == "devices" && last["devices"].size() == 1;
        }));
    }
    const auto recorded = lines.all();
    for (const auto* event : {"recording", "devices", "open", "start", "changed", "lost", "notice"})
    {
        INFO(event);
        CHECK(lines.has(event));
    }

    // Played back: up to the opening, then a stream opened on the device as it was, then the rest.
    std::vector<std::string> before, after;
    bool opened = false;
    for (const auto& line : recorded)
    {
        opened = opened || json::parse(line)["event"].stringOr("") == "open";
        (opened ? after : before).push_back(line);
    }
    auto fake = std::make_shared<FakeAudioSystem>();
    fake->replay(before, 1000.0);
    REQUIRE(fake->device("interface") != nullptr);
    fake->device("interface")->run({.speed = 1.0});
    Events events;
    AudioDevices devices(optionsFor(fake, events));
    CHECK(devices.defaultDevice("Fake", AudioDirection::input) == "interface");
    Recording recording(1 << 16);
    AudioError error;
    auto stream = devices.open("Fake", "interface", stereo(true, false), error);
    REQUIRE(stream != nullptr);
    REQUIRE(!stream->add(recording));
    REQUIRE(!stream->start());
    REQUIRE(soon([&] { return recording.frames > 4800; }));

    // The rate the recording ends with read back, the change and the loss met again, and the lists without it after.
    fake->device("interface")->update([](AudioDeviceInfo& info) { info.currentRate = 44100.0; });
    fake->replay(after);
    REQUIRE(events.waitFor(Kind::formatChanged));
    REQUIRE(events.waitFor(Kind::lost));
    CHECK(events.of(Kind::lost).front().loss == AudioLoss::unplugged);
    CHECK(soon([&] { return devices.devices("Fake").size() == 1; }));
}

// A stream on the interface through each session as the computer met it: its sleep, its waking, the interface lost and
// back, and the program opening it again as an editor carrying a take on does.
void replayOnInterface(const char* name, const std::string& system, const std::string& interface)
{
    const auto recorded = session(name);
    REQUIRE(!recorded.before.empty());
    REQUIRE(!recorded.after.empty());
    auto fake = std::make_shared<FakeAudioSystem>(system);
    fake->replay(recorded.before);
    REQUIRE(fake->device(interface) != nullptr);
    fake->device(interface)->run({.speed = 1.0});

    Events events;
    auto options = optionsFor(fake, events);
    options.streams.wakeDelay = 200ms;
    AudioDevices devices(options);
    Recording recording(1 << 18);
    AudioStreamRequest request;
    request.inputChannels = {0};
    AudioError error;
    auto stream = devices.open(system, interface, request, error);
    REQUIRE(stream != nullptr);
    REQUIRE(!stream->add(recording));
    REQUIRE(!stream->start());
    REQUIRE(soon([&] { return recording.frames > 4800; }));

    fake->replay(recorded.after);
    CHECK(met(events) == std::vector<Kind>{Kind::sleeping, Kind::woke, Kind::lost});
    REQUIRE(!events.of(Kind::lost).empty());
    CHECK(events.of(Kind::lost).front().loss == AudioLoss::unplugged);
    CHECK(stream->state() == AudioStream::State::lost);

    const auto had = recording.frames.load();
    REQUIRE(!stream->start());
    CHECK(soon([&] { return recording.frames > had + 4800; }));
}

TEST_CASE("A Mac's session played back: a stream on a USB interface stops as the Mac sleeps, opens again once it wakes, and is lost when the interface's hub is unplugged", "[timing]")
{
    // The hub's going slept the Mac, its lid closed, and the Mac's word of that sleep came only as it woke: the stream,
    // lost already, stays so.
    replayOnInterface("mac-sleep-unplug-headset.jsonl", "Core Audio", "usb-interface");
}

TEST_CASE("A Windows PC's session played back: a stream on a USB interface stops as the PC sleeps, opens again once it wakes, and is lost when the interface is unplugged", "[timing]")
{
    // Windows said nothing of the loss to the stream; the interface going from the list is the word.
    replayOnInterface("windows-sleep-unplug-headset.jsonl", "Windows Audio", "{0.0.1.00000000}.{interface-in}");
}

TEST_CASE("A Mac's session played back: an output following the default moves with it as a headset comes and goes, across sleep and the hub unplugged, and is never lost", "[timing]")
{
    const auto recorded = session("mac-sleep-unplug-headset.jsonl");
    REQUIRE(!recorded.before.empty());

    // Every device the session lists made first, each running whenever a stream opens it, and then the devices as
    // they were when it began.
    auto fake = std::make_shared<FakeAudioSystem>("Core Audio");
    std::set<std::string> ids;
    for (const auto* part : {&recorded.before, &recorded.after})
        for (const auto& line : *part)
        {
            const auto parsed = json::parse(line);
            if (const auto* listed = parsed["devices"].ifArray())
                for (const auto& device : *listed)
                    ids.insert(device["id"].stringOr(""));
        }
    auto whole = recorded.before;
    whole.insert(whole.end(), recorded.after.begin(), recorded.after.end());
    fake->replay(whole, 1e6);
    for (const auto& id : ids)
        if (auto* device = fake->device(id))
            device->run({.speed = 1.0});
    fake->replay(recorded.before, 1e6);
    REQUIRE(ids.size() == 9);
    REQUIRE(fake->defaultDevice(AudioDirection::output) == "usb-interface");

    Events events;
    auto options = optionsFor(fake, events);
    options.streams.wakeDelay = 200ms;
    AudioDevices devices(options);
    Recording recording(1 << 18);
    AudioStreamRequest request;
    request.outputChannels = {0, 1};
    request.followDefault = true;
    AudioError error;
    auto stream = devices.open("Core Audio", "usb-interface", request, error);
    REQUIRE(stream != nullptr);
    REQUIRE(!stream->add(recording));
    REQUIRE(!stream->start());
    REQUIRE(soon([&] { return recording.frames > 4800; }));

    fake->replay(recorded.after);
    INFO(events.count(Kind::moved) << " moves, " << events.count(Kind::sleeping) << " sleeps, ending on "
                                   << stream->deviceId());
    CHECK(events.count(Kind::lost) == 0);
    CHECK(events.count(Kind::moved) >= 3);
    CHECK(stream->deviceId() == fake->defaultDevice(AudioDirection::output));
    const auto had = recording.frames.load();
    CHECK(soon([&] { return recording.frames > had + 4800; }));
}

