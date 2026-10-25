// Records what this computer's audio systems do while a person sleeps the computer, unplugs an interface or connects
// a headset, a line of JSON for each thing, for FakeAudioSystem::replay to play back in tests, while an input is
// recorded from and what its stream says is written down beside it.
// Usage: symbolengine_devices_recorder <seconds> <file.jsonl> [part of the input's name] [system]

#include "Systems.h"

#include <symbolengine/core/Json.h>
#include <symbolengine/devices/AudioDevices.h>
#include <symbolengine/devices/RecordingAudioSystem.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

using namespace symbolengine;

namespace
{
const char* kindName(AudioEvent::Kind kind)
{
    using Kind = AudioEvent::Kind;
    switch (kind)
    {
        case Kind::started: return "started";
        case Kind::stopped: return "stopped";
        case Kind::lost: return "lost";
        case Kind::formatChanged: return "format changed";
        case Kind::dropout: return "dropout";
        case Kind::stalled: return "stalled";
        case Kind::resumed: return "resumed";
        case Kind::runaway: return "runaway";
        case Kind::noSound: return "no sound";
        case Kind::silentInput: return "silent input";
        case Kind::inputSound: return "input sound";
        case Kind::inputMuted: return "input muted";
        case Kind::inputUnmuted: return "input unmuted";
        case Kind::moved: return "moved";
        case Kind::idle: return "idle";
        case Kind::active: return "active";
        case Kind::sleeping: return "sleeping";
        case Kind::woke: return "woke";
        case Kind::devicesChanged: return "devices changed";
        case Kind::defaultChanged: return "default changed";
        case Kind::error: return "error";
    }
    return "event";
}

// The input's frames and loudest sample, counted on the audio thread.
class Counter final : public AudioCallback
{
public:
    void process(const AudioBlock& block) noexcept override
    {
        frames += block.frames;
        auto loudest = peak.load();
        for (int c = 0; c < block.inputChannels; ++c)
            for (int i = 0; i < block.frames; ++i)
                loudest = std::max(loudest, std::abs(block.inputs[c][i]));
        peak = loudest;
    }

    std::atomic<std::int64_t> frames{0};
    std::atomic<float> peak{0.0f};
};
}

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::printf("Usage: symbolengine_devices_recorder <seconds> <file.jsonl> [part of the input's name] [system]\n");
        return 2;
    }
    const auto seconds = std::atof(argv[1]);
    const std::string part = argc > 3 ? argv[3] : "";

    std::ofstream file(argv[2], std::ios::binary);
    std::mutex writing;
    const auto began = std::chrono::steady_clock::now();
    const auto write = [&](const std::string& line) {
        const std::lock_guard hold(writing);
        file << line << '\n';
        file.flush();
    };

    std::vector<std::shared_ptr<AudioSystem>> systems;
    for (auto& system : devices::detail::systemsOfThisComputer())
        systems.push_back(std::make_shared<RecordingAudioSystem>(system, write));
    if (systems.empty())
    {
        std::printf("This computer has no audio system.\n");
        return 1;
    }
    const std::string system = argc > 4 ? argv[4] : systems.front()->name();

    // What the streams and lists say, written beside what the systems did, and shown as it comes.
    AudioDevices::Options options;
    options.systems = systems;
    options.onEvent = [&](const AudioEvent& event) {
        const auto at = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
        write(json::write(Value(Object{{"seconds", at},
                                       {"event", "heard"},
                                       {"kind", kindName(event.kind)},
                                       {"system", event.system},
                                       {"device", event.deviceId},
                                       {"frames", event.frames},
                                       {"message", event.error.message}})));
        std::printf("%7.1f s  %s %s%s%s\n", at, kindName(event.kind), event.deviceName.c_str(),
                    event.error.message.empty() ? "" : ": ", event.error.message.c_str());
        std::fflush(stdout);
    };
    AudioDevices devices(options);

    const auto list = devices.devices(system);
    const auto fallback = devices.defaultDevice(system, AudioDirection::input);
    const AudioDeviceInfo* input = nullptr;
    for (const auto& device : list)
        if (device.inputs() > 0 && (part.empty() ? device.id == fallback : device.name.find(part) != std::string::npos))
            input = &device;
    if (input == nullptr)
    {
        std::printf("No input on %s%s%s.\n", system.c_str(), part.empty() ? "" : " named ", part.c_str());
        return 1;
    }

    Counter counter;
    AudioError error;
    AudioStreamRequest request;
    request.inputChannels = {0};
    auto stream = devices.open(system, input->id, request, error);
    if (stream == nullptr || stream->add(counter) || (error = stream->start()))
    {
        std::printf("%s didn't start: %s\n", input->name.c_str(), error.message.c_str());
        return 1;
    }
    std::printf("Recording what %s and its devices do for %.0f s, from %s, into %s.\n", system.c_str(), seconds,
                input->name.c_str(), argv[2]);
    std::fflush(stdout);

    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count() < seconds)
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

    const auto rate = stream->info().sampleRate;
    stream->stop();
    std::printf("%.3f s of input came in %.1f s, its loudest sample %.1f dBFS.\n",
                rate > 0.0 ? static_cast<double>(counter.frames.load()) / rate : 0.0, seconds,
                20.0 * std::log10(std::max(counter.peak.load(), 1e-12f)));
    return 0;
}
