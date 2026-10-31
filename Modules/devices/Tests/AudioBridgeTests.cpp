// The devices section's lessons for two clocks: an input carried to an output on another clock holds its delay,
// measures the drift and drops or doubles nothing, whether the clocks are a hundred parts per million apart or one
// percent, and starts again cleanly after the output stalls. The two clocks run in fake time, their blocks
// interleaved as their times come, so hours of drift take seconds.

#include <symbolengine/devices/AudioBridge.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <numbers>
#include <thread>
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

namespace
{
struct Run
{
    int channels = 2;
    double inputPace = 48000.0;
    double outputPace = 48000.0;
    double seconds = 60.0;
    int inputBlock = 512;
    int outputBlock = 512;
    double outputStartsAt = 0.0;
    double stallAt = -1.0;
    double stallFor = 0.0;
    bool times = true;
};

struct Measured
{
    AudioBridge::State end{};
    double realDelay = 0.0;        // what the output last played against when its input was captured
    double worstOffTarget = 0.0;   // how far the delay said strayed from its end value over the last 15 s
    bool settled = false;
    double driftLow = 0.0, driftHigh = 0.0;   // over the last 15 s
};

// The real bridge in fake time, with a ramp going in on every channel, so what the output plays says which input frame
// it is at, and so how far behind it really is.
Measured measured(AudioBridge& bridge, const Run& run)
{
    const auto channels = static_cast<std::size_t>(run.channels);
    AudioStreamInfo input;
    input.sampleRate = 48000.0;
    input.maxBlock = run.inputBlock;
    input.inputChannels = run.channels;
    bridge.input().prepare(input);
    AudioStreamInfo output;
    output.sampleRate = 48000.0;
    output.maxBlock = run.outputBlock;
    output.outputChannels = run.channels;

    std::vector<std::vector<float>> in(channels, std::vector<float>(static_cast<std::size_t>(run.inputBlock)));
    std::vector<std::vector<float>> out(channels, std::vector<float>(static_cast<std::size_t>(run.outputBlock)));
    std::vector<const float*> inputs;
    std::vector<float*> outputs;
    for (auto& channel : in)
        inputs.push_back(channel.data());
    for (auto& channel : out)
        outputs.push_back(channel.data());

    const double scale = 1.0 / (1 << 23);
    bool outputPrepared = false;
    double inputAt = 0.0, outputAt = run.outputStartsAt;
    std::int64_t frame = 0;
    Measured result;
    bool watching = false;

    while (std::min(inputAt, outputAt) < run.seconds)
    {
        if (inputAt <= outputAt)
        {
            for (auto& channel : in)
                for (std::size_t i = 0; i < channel.size(); ++i)
                    channel[i] = static_cast<float>(static_cast<double>(frame + static_cast<std::int64_t>(i)) * scale);
            AudioBlock given;
            given.inputs = inputs.data();
            given.inputChannels = run.channels;
            given.frames = run.inputBlock;
            given.inputTime =
                run.times ? static_cast<std::int64_t>((inputAt + run.inputBlock / run.inputPace) * 1e9) : -1;
            bridge.input().process(given);
            frame += run.inputBlock;
            inputAt += run.inputBlock / run.inputPace;
            continue;
        }

        if (!outputPrepared)
        {
            bridge.output().prepare(output);
            outputPrepared = true;
        }

        if (run.stallAt < 0.0 || outputAt < run.stallAt || outputAt >= run.stallAt + run.stallFor)
        {
            for (auto& channel : out)
                std::fill(channel.begin(), channel.end(), 0.0f);
            AudioBlock given;
            given.outputs = outputs.data();
            given.outputChannels = run.channels;
            given.frames = run.outputBlock;
            given.outputTime = run.times ? static_cast<std::int64_t>(outputAt * 1e9) : -1;
            bridge.output().process(given);

            if (out[0][0] != 0.0f)
                result.realDelay = outputAt - static_cast<double>(out[0][0]) / scale / run.inputPace;

            const auto now = bridge.state();
            result.end = now;
            if (outputAt >= run.seconds - 15.0)
            {
                if (!watching)
                {
                    watching = true;
                    result.driftLow = result.driftHigh = now.drift;
                }
                result.driftLow = std::min(result.driftLow, now.drift);
                result.driftHigh = std::max(result.driftHigh, now.drift);
            }
        }
        outputAt += run.outputBlock / run.outputPace;
    }

    return result;
}
}

TEST_CASE("At any number of channels the bridge's delay is what it says, from the start, after a late output and after "
          "a stall")
{
    for (const int channels : {1, 2, 5})
    {
        CAPTURE(channels);
        struct Case
        {
            double late;
            double stallFor;
        };
        for (const Case& start : {Case{0.0, 0.0}, Case{0.1, 0.0}, Case{0.0, 2.0}})
        {
            CAPTURE(start.late, start.stallFor);
            AudioBridge bridge({.channels = channels, .delay = std::chrono::milliseconds(30)});
            Run run;
            run.channels = channels;
            run.seconds = 20.0;
            run.outputStartsAt = start.late;
            run.stallAt = start.stallFor > 0.0 ? 5.0 : -1.0;
            run.stallFor = start.stallFor;
            const auto got = measured(bridge, run);
            CHECK(got.end.settled);
            CHECK(std::abs(got.end.delaySeconds - 0.030) < 0.001);

            // The resampler adds its own lookahead of a few hundredths of a millisecond on top.
            CHECK(std::abs(got.realDelay - got.end.delaySeconds) < 0.002);
            CHECK(got.end.underruns == 0);
        }
    }
}

TEST_CASE("Blocks of any size give a steady bridge, its delay at least what they need")
{
    for (const int block : {64, 512, 2048, 4096, 8192, 16384})
    {
        CAPTURE(block);
        AudioBridge bridge({.channels = 1, .delay = std::chrono::milliseconds(30)});
        Run run;
        run.channels = 1;
        run.inputPace = 48000.0 * (1.0 + 100e-6);

        // The loop's steps are the blocks', so the longest take the longest to settle.
        run.seconds = block >= 8192 ? 120.0 : 45.0;
        run.inputBlock = run.outputBlock = block;
        const auto got = measured(bridge, run);
        INFO("delay " << got.end.delaySeconds << " real " << got.realDelay << " drift " << got.driftLow << " to "
                      << got.driftHigh);
        CHECK(got.end.underruns == 0);
        CHECK(got.end.overruns == 0);
        CHECK(got.end.settled);

        // The drift measured is the clocks', steadily, with no ringing left.
        CHECK(std::abs(got.end.drift - 100.0) < 15.0);
        CHECK(got.driftHigh - got.driftLow < 30.0);

        // An input block and an output block each have to fit in what is held; less than that can't be.
        CHECK(got.end.delaySeconds >= 0.030 - 0.001);
        CHECK(got.end.delaySeconds >= 2.0 * block / 48000.0);
        CHECK(std::abs(got.realDelay - got.end.delaySeconds) < 0.002);
    }
}

TEST_CASE("Blocks of different sizes either side give a steady bridge")
{
    struct Sizes
    {
        int input;
        int output;
    };
    for (const Sizes& sizes : {Sizes{64, 2048}, Sizes{2048, 64}, Sizes{480, 512}})
    {
        CAPTURE(sizes.input, sizes.output);
        AudioBridge bridge({.channels = 1, .delay = std::chrono::milliseconds(30)});
        Run run;
        run.channels = 1;
        run.inputPace = 48000.0 * (1.0 - 100e-6);
        run.seconds = 45.0;
        run.inputBlock = sizes.input;
        run.outputBlock = sizes.output;
        const auto got = measured(bridge, run);
        CHECK(got.end.underruns == 0);
        CHECK(got.end.overruns == 0);
        CHECK(got.end.settled);
        CHECK(std::abs(got.end.drift + 100.0) < 15.0);
    }
}

TEST_CASE("Streams that give no times hold the bridge steady too, its ratio wavering a little")
{
    AudioBridge bridge({.channels = 1, .delay = std::chrono::milliseconds(30)});
    Run run;
    run.channels = 1;
    run.inputPace = 48000.0 * (1.0 + 100e-6);
    run.seconds = 90.0;
    run.times = false;
    run.inputBlock = 480;
    run.outputBlock = 512;
    const auto got = measured(bridge, run);
    INFO("drift " << got.driftLow << " to " << got.driftHigh);
    CHECK(got.end.underruns == 0);
    CHECK(got.end.overruns == 0);
    CHECK(std::abs(got.end.drift - 100.0) < 100.0);
}

TEST_CASE("An input coming to its stream after the output, at another rate, is cut off for the rate it has")
{
    // At 96 kHz, a tone above what 48 kHz can hold, which would fold down to 18 kHz.
    const auto folded = [](bool inputFirst, bool changes) {
        AudioBridge bridge({.channels = 1, .delay = std::chrono::milliseconds(30)});
        AudioStreamInfo input;
        input.sampleRate = changes ? 44100.0 : 96000.0;
        input.maxBlock = 512;
        input.inputChannels = 1;
        AudioStreamInfo output;
        output.sampleRate = 48000.0;
        output.maxBlock = 512;
        output.outputChannels = 1;

        if (inputFirst)
            bridge.input().prepare(input);
        bridge.output().prepare(output);
        if (!inputFirst)
            bridge.input().prepare(input);
        if (changes)
        {
            input.sampleRate = 96000.0;
            bridge.input().prepare(input);
        }

        std::vector<float> in(512), out(512);
        const float* inputs[]{in.data()};
        float* outputs[]{out.data()};
        double inputAt = 0.0, outputAt = 0.0, energy = 0.0;
        std::int64_t frame = 0;
        std::int64_t counted = 0;

        while (outputAt < 8.0)
        {
            if (inputAt <= outputAt)
            {
                for (std::size_t i = 0; i < in.size(); ++i)
                    in[i] = 0.5f * static_cast<float>(
                                       std::sin(2.0 * std::numbers::pi * 30000.0 *
                                                static_cast<double>(frame + static_cast<std::int64_t>(i)) / 96000.0));
                AudioBlock given;
                given.inputs = inputs;
                given.inputChannels = 1;
                given.frames = 512;
                given.inputTime = static_cast<std::int64_t>((inputAt + 512.0 / 96000.0) * 1e9);
                bridge.input().process(given);
                frame += 512;
                inputAt += 512.0 / 96000.0;
                continue;
            }

            std::fill(out.begin(), out.end(), 0.0f);
            AudioBlock given;
            given.outputs = outputs;
            given.outputChannels = 1;
            given.frames = 512;
            given.outputTime = static_cast<std::int64_t>(outputAt * 1e9);
            bridge.output().process(given);
            if (outputAt > 2.0)
            {
                for (const auto sample : out)
                    energy += static_cast<double>(sample) * sample;
                counted += 512;
            }
            outputAt += 512.0 / 48000.0;
        }

        return 10.0 * std::log10(std::max(energy / static_cast<double>(std::max<std::int64_t>(counted, 1)), 1e-30));
    };

    CHECK(folded(true, false) < -90.0);
    CHECK(folded(false, false) < -90.0);
    CHECK(folded(true, true) < -90.0);
}

TEST_CASE("The channels of a block stay together whichever thread is held up where")
{
    // Real threads, an input writing long blocks and an output taking short ones often, with every channel the same, so
    // any output block whose channels differ has parted them.
    constexpr int channels = 8;
    constexpr int inputBlock = 8192;
    AudioBridge bridge(
        {.channels = channels, .delay = std::chrono::milliseconds(5), .capacity = std::chrono::milliseconds(2000)});
    AudioStreamInfo input;
    input.sampleRate = 48000.0;
    input.maxBlock = inputBlock;
    input.inputChannels = channels;
    bridge.input().prepare(input);
    AudioStreamInfo output;
    output.sampleRate = 48000.0;
    output.maxBlock = 16;
    output.outputChannels = channels;
    bridge.output().prepare(output);

    std::atomic<bool> running{true};
    std::thread inputThread([&] {
        std::vector<std::vector<float>> in(channels, std::vector<float>(inputBlock));
        std::vector<const float*> pointers;
        for (auto& channel : in)
            pointers.push_back(channel.data());
        std::int64_t frame = 0;
        while (running)
        {
            for (std::size_t i = 0; i < static_cast<std::size_t>(inputBlock); ++i, ++frame)
                for (auto& channel : in)
                    channel[i] = static_cast<float>(frame % 1000) * 1e-3f;
            AudioBlock given;
            given.inputs = pointers.data();
            given.inputChannels = channels;
            given.frames = inputBlock;
            bridge.input().process(given);
        }
    });

    std::vector<std::vector<float>> out(channels, std::vector<float>(16));
    std::vector<float*> pointers;
    for (auto& channel : out)
        pointers.push_back(channel.data());
    std::int64_t parted = 0, sounding = 0;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < until)
    {
        for (auto& channel : out)
            std::fill(channel.begin(), channel.end(), 0.0f);
        AudioBlock given;
        given.outputs = pointers.data();
        given.outputChannels = channels;
        given.frames = 16;
        bridge.output().process(given);
        for (std::size_t c = 1; c < out.size(); ++c)
            for (std::size_t i = 0; i < 16; ++i)
                if (std::abs(out[c][i] - out[0][i]) > 0.01f)
                    ++parted;
        sounding += out[0][8] != 0.0f ? 1 : 0;
    }
    running = false;
    inputThread.join();
    CHECK(sounding > 0);
    CHECK(parted == 0);
}
