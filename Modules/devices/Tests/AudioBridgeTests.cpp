// The devices section's lessons for two clocks: an input carried to an output on another clock holds its delay,
// measures the drift and drops or doubles nothing, whether the clocks are a hundred parts per million apart or one
// percent, and starts again cleanly after the output stalls. The two clocks run in fake time, their blocks
// interleaved as their times come, so hours of drift take seconds.

#include <symbolengine/devices/AudioBridge.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

using namespace symbolengine;

namespace
{
struct Clocks
{
    double inputPace = 48000.0;
    double outputPace = 48000.0;
    double seconds = 60.0;
    int block = 512;
    double stallAt = -1.0;
    double stallFor = 0.0;
};

struct Heard
{
    std::vector<float> samples;
    double settledAt = -1.0;
    double worstDelayAfter = 0.0;
    AudioBridge::State end{};
};

// A 50 Hz sine at half scale in, numbered by its frames, and what the output played, with when the delay settled and
// how far it strayed from the target after the time given.
Heard run(AudioBridge& bridge, const Clocks& clocks, double watchAfter, double target)
{
    const auto block = static_cast<std::size_t>(clocks.block);
    AudioStreamInfo inputInfo;
    inputInfo.sampleRate = 48000.0;
    inputInfo.maxBlock = clocks.block;
    inputInfo.inputChannels = 1;
    bridge.input().prepare(inputInfo);
    AudioStreamInfo outputInfo;
    outputInfo.sampleRate = 48000.0;
    outputInfo.maxBlock = clocks.block;
    outputInfo.outputChannels = 1;
    bridge.output().prepare(outputInfo);

    std::vector<float> in(block), out(block);
    const float* inputs[]{in.data()};
    float* outputs[]{out.data()};
    Heard heard;
    heard.samples.reserve(static_cast<std::size_t>(clocks.seconds * clocks.outputPace) + block);
    double inputAt = 0.0, outputAt = 0.0;
    std::int64_t frame = 0;
    while (std::min(inputAt, outputAt) < clocks.seconds)
    {
        if (inputAt <= outputAt)
        {
            for (std::size_t i = 0; i < block; ++i)
                in[i] = 0.5f * static_cast<float>(std::sin(2.0 * std::numbers::pi * 50.0 *
                                                           static_cast<double>(frame + static_cast<std::int64_t>(i)) / 48000.0));
            // Each block's capture time, its last frame's, on the fake clock both share.
            AudioBlock given;
            given.inputs = inputs;
            given.inputChannels = 1;
            given.frames = clocks.block;
            given.inputTime = static_cast<std::int64_t>((inputAt + static_cast<double>(clocks.block) / clocks.inputPace) * 1e9);
            bridge.input().process(given);
            frame += clocks.block;
            inputAt += static_cast<double>(clocks.block) / clocks.inputPace;
            continue;
        }

        const bool stalled = clocks.stallAt >= 0.0 && outputAt >= clocks.stallAt && outputAt < clocks.stallAt + clocks.stallFor;
        if (!stalled)
        {
            std::fill(out.begin(), out.end(), 0.0f);
            AudioBlock given;
            given.outputs = outputs;
            given.outputChannels = 1;
            given.frames = clocks.block;
            given.outputTime = static_cast<std::int64_t>(outputAt * 1e9);
            bridge.output().process(given);
            heard.samples.insert(heard.samples.end(), out.begin(), out.end());
            const auto now = bridge.state();
            if (now.settled && heard.settledAt < 0.0)
                heard.settledAt = outputAt;
            if (outputAt >= watchAfter)
                heard.worstDelayAfter = std::max(heard.worstDelayAfter, std::abs(now.delaySeconds - target));
        }
        outputAt += static_cast<double>(clocks.block) / clocks.outputPace;
    }
    heard.end = bridge.state();
    return heard;
}

// The largest step between one sample's slope and the next, over the samples from `from`: a 50 Hz sine's is about
// 2e-5, and a frame dropped or doubled makes one of thousandths.
double largestStep(const std::vector<float>& samples, std::size_t from)
{
    double largest = 0.0;
    for (std::size_t i = std::max<std::size_t>(from, 2); i < samples.size(); ++i)
        largest = std::max(largest, std::abs(static_cast<double>(samples[i]) - 2.0 * samples[i - 1] + samples[i - 2]));
    return largest;
}

// Where the sound starts: the bridge plays nothing until its buffer holds the delay.
std::size_t firstSound(const std::vector<float>& samples)
{
    const auto found = std::find_if(samples.begin(), samples.end(), [](float s) { return s != 0.0f; });
    return static_cast<std::size_t>(found - samples.begin());
}
}

TEST_CASE("An input a hundred parts per million fast is carried to an output for minutes, its delay held and its drift measured")
{
    AudioBridge bridge({.channels = 1, .delay = std::chrono::milliseconds(30)});
    const auto heard = run(bridge, {.inputPace = 48000.0 * (1.0 + 100e-6), .seconds = 240.0}, 60.0, 0.030);
    INFO("settled at " << heard.settledAt << " s, drift " << heard.end.drift << " ppm, delay " << heard.end.delaySeconds
                       << " s, strayed " << heard.worstDelayAfter << " s after a minute");
    CHECK(heard.settledAt >= 0.0);
    CHECK(heard.settledAt < 30.0);
    CHECK(std::abs(heard.end.drift - 100.0) < 10.0);
    CHECK(heard.worstDelayAfter < 0.002);
    CHECK(heard.end.underruns == 0);
    CHECK(heard.end.overruns == 0);
    CHECK(largestStep(heard.samples, firstSound(heard.samples) + 4800) < 1e-3);
}

TEST_CASE("Inputs one percent fast and one percent slow settle within a minute and never run the buffer over or dry")
{
    for (const double apart : {0.01, -0.01})
    {
        INFO("input " << apart * 100.0 << "% apart");
        AudioBridge bridge({.channels = 1, .delay = std::chrono::milliseconds(30)});
        const auto heard = run(bridge, {.inputPace = 48000.0 * (1.0 + apart), .seconds = 90.0}, 60.0, 0.030);
        INFO("settled at " << heard.settledAt << " s, drift " << heard.end.drift << " ppm, strayed "
                           << heard.worstDelayAfter << " s after a minute");
        CHECK(heard.settledAt >= 0.0);
        CHECK(heard.settledAt < 60.0);
        CHECK(std::abs(heard.end.drift - apart * 1e6) < 0.03 * std::abs(apart) * 1e6);
        CHECK(heard.worstDelayAfter < 0.002);
        CHECK(heard.end.underruns == 0);
        CHECK(heard.end.overruns == 0);
        CHECK(largestStep(heard.samples, firstSound(heard.samples) + 4800) < 1e-3);
    }
}

TEST_CASE("An output that stalls longer than the buffer holds starts again from the delay and settles again")
{
    AudioBridge bridge({.channels = 1, .delay = std::chrono::milliseconds(30), .capacity = std::chrono::milliseconds(500)});
    const auto heard = run(bridge, {.inputPace = 48000.0 * (1.0 + 100e-6), .seconds = 90.0, .stallAt = 30.0, .stallFor = 2.0},
                           70.0, 0.030);
    INFO("drift " << heard.end.drift << " ppm, strayed " << heard.worstDelayAfter << " s after 70 s");
    CHECK(heard.end.overruns == 1);
    CHECK(heard.end.underruns == 0);
    CHECK(heard.end.settled);
    CHECK(heard.worstDelayAfter < 0.002);
}
