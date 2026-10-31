#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace symbolengine
{

/** Audio from one rate to another, a stretch at a time, by band-limited interpolation: each output sample is the
    input's signal at its own instant, from a Kaiser-windowed sinc 64 zero crossings either side, cut off below the
    lower of the two Nyquist frequencies, so going down a rate what lies past the new Nyquist is 90 dB down from a
    quarter of a kilohertz past it at 48 kHz, and what lies at 20 kHz loses less than 0.2 dB. A 1 kHz tone comes out
    within -110 dB of the ideal, and one at 18 kHz within -100 dB. The ratio can be changed between stretches by small amounts, as
    correcting the drift between two clocks does. Nothing is allocated once it is made, so it runs on an audio thread
    too. */
class Resampler
{
public:
    /** ratio is the output's rate over the input's; maxInput the most input frames given in one call. */
    Resampler(int channels, double ratio, std::size_t maxInput = 8192);

    struct Done
    {
        std::size_t consumed = 0;
        std::size_t produced = 0;
    };

    /** Takes up to inputFrames of each channel and gives up to outputFrames, stopping when either runs out; returns
        how many it took and gave. Input it takes is kept until the output that needs it has been given. */
    Done process(const float* const* input, std::size_t inputFrames, float* const* output,
                 std::size_t outputFrames) noexcept;

    /** A new ratio for what follows, near the first; the cut-off stays where the first ratio put it. */
    void setRatio(double ratio) noexcept;
    double ratio() const noexcept { return outputOverInput; }

    /** How many input frames past a sample's instant are needed before it can be given. */
    int lookahead() const noexcept { return reach; }

    /** Starts again from silence, at the same ratio. */
    void reset() noexcept;

private:
    double kernel(double x) const noexcept;

    int channelCount;
    double outputOverInput;
    double step;

    // The kernel, tabulated over [-reach, reach] input samples at `resolution` points a sample.
    int reach = 0;
    int resolution = 512;
    std::vector<double> table;

    // Input kept, each channel from input frame `base`; the next output's instant is `whole` plus `fraction`.
    std::vector<std::vector<float>> kept;
    std::size_t filled = 0;
    std::int64_t base = 0;
    std::int64_t whole = 0;
    double fraction = 0.0;
};

}
