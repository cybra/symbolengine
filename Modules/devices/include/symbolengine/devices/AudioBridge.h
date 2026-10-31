#pragma once

#include "AudioStream.h"

#include <chrono>
#include <memory>

namespace symbolengine
{

/** Carries an input on one device to an output on another, whose clocks run apart, as an input heard through a
    computer's speakers is: never by a buffer of fixed size, which in time runs over or dry, nor at the output's pace
    alone. The input's frames go into a buffer from its own thread, and the output takes them through an adaptive
    resampler whose ratio follows how full the buffer is against its target, so the delay between the two stays where
    it was set over hours and the drift of the two clocks is measured. A recording is written from the input's own
    stream, never through here, so its samples stay the interface's own. Add input() to the input's stream and output()
    to the output's; output() adds the input, scaled by its gain, to what the output's other callbacks wrote, each
    channel to its own. */
class AudioBridge
{
public:
    struct Options
    {
        int channels = 2;

        /** How far behind the input the output plays, which the buffer is held to, at least what an input block and
            an output block need held at once. */
        std::chrono::milliseconds delay{30};

        /** The most the buffer holds; an output that stops taking for longer has the buffer start again from the
            delay. */
        std::chrono::milliseconds capacity{500};

        float gain = 1.0f;
    };

    explicit AudioBridge(Options options);
    AudioBridge() : AudioBridge(Options{}) {}
    ~AudioBridge();

    AudioBridge(const AudioBridge&) = delete;
    AudioBridge& operator=(const AudioBridge&) = delete;

    AudioCallback& input() noexcept;
    AudioCallback& output() noexcept;

    struct State
    {
        /** Parts per million the input's clock runs fast against the output's, as measured once the delay settled. */
        double drift = 0.0;

        /** How far behind the input the output plays now. */
        double delaySeconds = 0.0;

        /** The delay is within a millisecond of where it is held. */
        bool settled = false;

        /** Times the buffer ran dry or over and started again from the delay. */
        std::int64_t underruns = 0;
        std::int64_t overruns = 0;
    };

    /** From any thread. */
    State state() const noexcept;

    struct Impl;

private:
    std::unique_ptr<Impl> impl;
};

}
