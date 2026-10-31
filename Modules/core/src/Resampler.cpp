#include "symbolengine/core/Resampler.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace symbolengine
{

namespace
{
constexpr int crossings = 64;
constexpr double beta = 10.0;

// The zeroth-order modified Bessel function, for the Kaiser window.
double bessel0(double x)
{
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 64; ++k)
    {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
        if (term < sum * 1e-17)
            break;
    }
    return sum;
}
}

Resampler::Resampler(int channels, double ratio, std::size_t maxInput)
    : channelCount(std::max(1, channels)), outputOverInput(ratio), step(1.0 / ratio)
{
    // Going down, the cut-off follows the output's Nyquist and the kernel widens to keep its zero crossings.
    const double cutoff = 0.94 * std::min(1.0, ratio);
    reach = static_cast<int>(std::ceil(crossings / cutoff));

    const auto points = static_cast<std::size_t>(2 * reach * resolution + 1);
    table.resize(points + 1);
    const double norm = bessel0(beta);
    for (std::size_t i = 0; i < points; ++i)
    {
        const double x = static_cast<double>(i) / resolution - reach;
        const double t = x / reach;
        const double window = std::abs(t) >= 1.0 ? 0.0 : bessel0(beta * std::sqrt(1.0 - t * t)) / norm;
        const double arg = std::numbers::pi * cutoff * x;
        const double sinc = x == 0.0 ? 1.0 : std::sin(arg) / arg;
        table[i] = cutoff * sinc * window;
    }
    table[points] = 0.0;

    kept.assign(static_cast<std::size_t>(channelCount),
                std::vector<float>(static_cast<std::size_t>(4 * reach) + maxInput, 0.0f));
    reset();
}

void Resampler::reset() noexcept
{
    // Silence before the first input frame, so the first output has the history it needs.
    for (auto& channel : kept)
        std::fill(channel.begin(), channel.end(), 0.0f);
    filled = static_cast<std::size_t>(reach);
    base = -reach;
    whole = 0;
    fraction = 0.0;
}

void Resampler::setRatio(double ratio) noexcept
{
    outputOverInput = ratio;
    step = 1.0 / ratio;
}

double Resampler::kernel(double x) const noexcept
{
    const double at = (x + reach) * resolution;
    if (at <= 0.0 || at >= static_cast<double>(table.size() - 1))
        return 0.0;
    const auto i = static_cast<std::size_t>(at);
    const double between = at - static_cast<double>(i);
    return table[i] + (table[i + 1] - table[i]) * between;
}

Resampler::Done Resampler::process(const float* const* input, std::size_t inputFrames, float* const* output,
                                   std::size_t outputFrames) noexcept
{
    Done done;
    const auto room = kept.front().size();

    // The input the next output needs: up to `reach` frames past its instant.
    const auto needed = [&] { return static_cast<std::size_t>(whole + reach - base + 1); };

    while (done.produced < outputFrames)
    {
        while (filled < needed() && done.consumed < inputFrames)
        {
            if (filled == room)
            {
                // Room made by dropping what no output still needs.
                const auto drop = whole - reach + 1 - base;
                if (drop <= 0)
                    break;
                for (auto& channel : kept)
                    std::copy(channel.begin() + static_cast<std::ptrdiff_t>(drop),
                              channel.begin() + static_cast<std::ptrdiff_t>(filled), channel.begin());
                filled -= static_cast<std::size_t>(drop);
                base += drop;
            }
            const auto take = std::min(inputFrames - done.consumed, std::min(room - filled, needed() - filled));
            for (int c = 0; c < channelCount; ++c)
                std::copy(input[c] + done.consumed, input[c] + done.consumed + take,
                          kept[static_cast<std::size_t>(c)].begin() + static_cast<std::ptrdiff_t>(filled));
            filled += take;
            done.consumed += take;
        }
        if (filled < needed())
            break;

        // The sample at whole + fraction, from the input either side.
        const auto first = whole - reach + 1;
        for (int c = 0; c < channelCount; ++c)
        {
            const auto& channel = kept[static_cast<std::size_t>(c)];
            double sum = 0.0;
            for (auto k = first; k <= whole + reach; ++k)
                sum += channel[static_cast<std::size_t>(k - base)] *
                       kernel(static_cast<double>(k - whole) - fraction);
            output[c][done.produced] = static_cast<float>(sum);
        }
        ++done.produced;

        fraction += step;
        const auto skip = std::floor(fraction);
        whole += static_cast<std::int64_t>(skip);
        fraction -= skip;
    }
    return done;
}

}
