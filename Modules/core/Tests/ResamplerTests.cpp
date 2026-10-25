#include <symbolengine/core/Resampler.h>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <numbers>
#include <random>
#include <vector>

using namespace symbolengine;

namespace
{
// A tone at one rate resampled to another in chunks of random sizes, its output measured against the tone at each
// output sample's own instant, past the start's settling: the error's level in dB below full scale.
double errorOf(double frequency, double from, double to, double seconds = 1.0)
{
    const auto frames = static_cast<std::size_t>(seconds * from);
    std::vector<float> input(frames);
    for (std::size_t i = 0; i < frames; ++i)
        input[i] = static_cast<float>(0.5 * std::sin(2.0 * std::numbers::pi * frequency * static_cast<double>(i) / from));

    Resampler resampler(1, to / from);
    std::vector<float> output(static_cast<std::size_t>(seconds * to) + 16);
    std::minstd_rand random(9);
    std::size_t taken = 0, given = 0;
    while (taken < frames && given < output.size())
    {
        const float* in[1]{input.data() + taken};
        float* out[1]{output.data() + given};
        const auto chunk = std::min<std::size_t>(frames - taken, 1 + random() % 700);
        const auto done = resampler.process(in, chunk, out, std::min<std::size_t>(output.size() - given, 1 + random() % 700));
        taken += done.consumed;
        given += done.produced;
    }

    double error = 0.0;
    std::size_t counted = 0;
    const auto settle = static_cast<std::size_t>(0.05 * to);
    for (std::size_t j = settle; j + settle < given; ++j)
    {
        const auto ideal = 0.5 * std::sin(2.0 * std::numbers::pi * frequency * static_cast<double>(j) / to);
        error += (output[j] - ideal) * (output[j] - ideal);
        ++counted;
    }
    return 10.0 * std::log10(error / static_cast<double>(counted) + 1e-30);
}

double levelOf(const std::vector<float>& samples, std::size_t from, std::size_t to)
{
    double sum = 0.0;
    for (auto i = from; i < to; ++i)
        sum += samples[i] * samples[i];
    return 10.0 * std::log10(sum / static_cast<double>(to - from) + 1e-30);
}
}

TEST_CASE("Resampler gives a tone within -110 dB of the ideal, up and down between the common rates")
{
    CHECK(errorOf(1000.0, 44100.0, 48000.0) < -110.0);
    CHECK(errorOf(1000.0, 48000.0, 44100.0) < -110.0);
    CHECK(errorOf(1000.0, 48000.0, 96000.0) < -110.0);
    CHECK(errorOf(1000.0, 96000.0, 48000.0) < -110.0);
    CHECK(errorOf(997.0, 48000.0, 48000.0) < -110.0);
}

TEST_CASE("Resampler keeps a tone at 18 kHz within -100 dB of the ideal")
{
    CHECK(errorOf(18000.0, 48000.0, 44100.0) < -100.0);
    CHECK(errorOf(18000.0, 44100.0, 48000.0) < -100.0);
}

TEST_CASE("Resampler going down a rate leaves nothing of a tone above the new Nyquist")
{
    // 30 kHz at 96 kHz, taken to 44.1 kHz, where it would alias to 14.1 kHz.
    const double from = 96000.0, to = 44100.0;
    std::vector<float> input(static_cast<std::size_t>(from));
    for (std::size_t i = 0; i < input.size(); ++i)
        input[i] = static_cast<float>(0.5 * std::sin(2.0 * std::numbers::pi * 30000.0 * static_cast<double>(i) / from));
    Resampler resampler(1, to / from);
    std::vector<float> output(static_cast<std::size_t>(to));
    const float* in[1]{input.data()};
    float* out[1]{output.data()};
    const auto done = resampler.process(in, input.size(), out, output.size());
    REQUIRE(done.produced > 40000);
    CHECK(levelOf(output, 2000, done.produced - 2000) < -110.0);
}

TEST_CASE("Resampler's output keeps time with its input over ten seconds")
{
    const double from = 44100.0, to = 48000.0;
    Resampler resampler(2, to / from);
    std::vector<float> left(4410), right(4410), outLeft(6000), outRight(6000);
    std::size_t produced = 0, consumed = 0;
    for (int i = 0; i < 100; ++i)
    {
        std::size_t taken = 0;
        while (taken < left.size())
        {
            const float* in[2]{left.data() + taken, right.data() + taken};
            float* out[2]{outLeft.data(), outRight.data()};
            const auto done = resampler.process(in, left.size() - taken, out, outLeft.size());
            taken += done.consumed;
            produced += done.produced;
        }
        consumed += taken;
    }
    const auto expected = static_cast<double>(consumed) * to / from;
    CHECK(std::abs(static_cast<double>(produced) - expected) <= resampler.lookahead() * to / from + 2.0);
}

TEST_CASE("Resampler's ratio moved by drift's amounts while it runs makes no click")
{
    const double rate = 48000.0;
    Resampler resampler(1, 1.0);
    std::vector<float> input(512), output(600);
    double phase = 0.0, largestStep = 0.0;
    float previous = 0.0f;
    std::size_t given = 0;
    for (int block = 0; block < 400; ++block)
    {
        for (auto& sample : input)
        {
            sample = static_cast<float>(0.5 * std::sin(phase));
            phase += 2.0 * std::numbers::pi * 1000.0 / rate;
        }
        resampler.setRatio(1.0 + 1e-4 * std::sin(block * 0.1));
        const float* in[1]{input.data()};
        float* out[1]{output.data()};
        const auto done = resampler.process(in, input.size(), out, output.size());
        for (std::size_t i = 0; i < done.produced; ++i, ++given)
        {
            if (given > 200)
                largestStep = std::max(largestStep, std::abs(static_cast<double>(output[i] - previous)));
            previous = output[i];
        }
    }
    // A 1 kHz tone at half scale steps at most 0.066 a sample at 48 kHz.
    CHECK(largestStep < 0.07);
}
