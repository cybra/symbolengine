// A small program that lists the computer's audio devices and, only when given --play, plays a quiet 440 Hz tone for
// two seconds on the default output and says what happened. Listing opens nothing, so it works with no devices at all.

#include <symbolengine/devices.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <string_view>
#include <thread>

using namespace symbolengine;
using namespace std::chrono_literals;

namespace
{

// A sine that fades in and out, as one that starts or stops at full level clicks. All it does on the audio thread is
// arithmetic. Outputs arrive silent, so once it has ended it writes nothing.
struct Tone final : AudioCallback
{
    void process(const AudioBlock& block) noexcept override
    {
        constexpr auto level = 0.03, length = 2.0;   // about -30 dBFS, for two seconds

        for (auto i = 0; i < block.frames && seconds < length; ++i)
        {
            const auto fade = std::min({1.0, seconds / 0.01, (length - seconds) / 0.01});
            const auto sample = static_cast<float>(level * fade * std::sin(2.0 * std::numbers::pi * 440.0 * seconds));

            for (auto c = 0; c < block.outputChannels; ++c)
                block.outputs[c][i] = sample;

            seconds += 1.0 / block.sampleRate;
        }

        ended = seconds >= length;
    }

    double seconds = 0.0;
    std::atomic<bool> ended{false};
};

void report(const AudioEvent& event)
{
    if (event.kind == AudioEvent::Kind::dropout)   // its frames are -1 when the system doesn't say how many
        std::printf("A dropout of %lld frames at frame %lld.\n", static_cast<long long>(event.frames),
                    static_cast<long long>(event.position));
    else if (event.kind == AudioEvent::Kind::stalled)
        std::printf("The device stopped calling back at frame %lld.\n", static_cast<long long>(event.position));
    else if (event.error)   // a device lost, or no sound from it
        std::printf("%s\n", event.error.message.c_str());
}

bool play(AudioDevices& devices)
{
    // A new setup follows the first system's default output, on its first two channels or its only one.
    const auto output = devices.resolve({}).output;

    if (output.id.empty() || !output.exact)
    {
        std::printf("There is no default output to play on.\n");
        return false;
    }

    AudioStreamRequest request;
    request.outputChannels = output.channels;

    Tone tone;
    AudioError error;
    auto stream = devices.open(devices.systems().front(), output.id, request, error);

    if (stream == nullptr || (error = stream->add(tone)) || (error = stream->start()))
    {
        std::printf("%s didn't start. %s\n", output.name.c_str(), error.message.c_str());
        return false;
    }

    // The tone ends itself. A device can stop calling back while it says it runs, so the wait has a limit too.
    std::printf("Playing a quiet tone on %s.\n", output.name.c_str());
    const auto until = std::chrono::steady_clock::now() + 5s;

    while (!tone.ended && stream->state() == AudioStream::State::running && std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for(10ms);

    // What the tone wrote last is still on its way to the speakers.
    std::this_thread::sleep_for(200ms);
    stream->stop();
    std::printf("Dropouts: %lld\n", static_cast<long long>(stream->statistics().dropouts));
    return tone.ended;
}

}

int main(int argc, char** argv)
{
    const auto playing = argc == 2 && std::string_view(argv[1]) == "--play";

    // Events come on a thread of the module's own, as this program gives them no other.
    AudioDevices::Options options;
    options.onEvent = report;

    std::size_t found = 0;
    auto failed = false;

    {
        AudioDevices devices(options);

        for (const auto& system : devices.systems())
            for (const auto& device : devices.devices(system))
                std::printf("%zu. %s: %s, %d in, %d out, %.0f Hz%s\n", ++found, system.c_str(), device.name.c_str(),
                            device.inputs(), device.outputs(), device.currentRate,
                            device.id == devices.defaultDevice(system, AudioDirection::output) ? ", default out" : "");

        failed = playing && !play(devices);
    }

    // AudioDevices delivers what is still on its way before it goes, so this comes last.
    std::printf("Devices found: %zu\n", found);
    return failed ? 1 : 0;
}
