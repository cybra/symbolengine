#pragma once

#include "AudioDevice.h"
#include "AudioSystem.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace symbolengine
{

/** One block of a stream, given to each callback on the audio thread. */
struct AudioBlock
{
    /** One buffer for each channel the stream took, in its order. */
    const float* const* inputs = nullptr;
    int inputChannels = 0;

    /** One buffer for each output channel, silent as it arrives. */
    float* const* outputs = nullptr;
    int outputChannels = 0;

    /** From 1 to the stream's maxBlock. */
    int frames = 0;

    /** Where the block's first input and output frames fall since the stream started, counting every frame lost as
        well as every frame delivered, so a recording placed by them keeps real time. */
    std::int64_t inputPosition = 0;
    std::int64_t outputPosition = 0;

    /** When its input was captured and when its output will be heard, in nanoseconds on the system's steady clock,
        or -1 when the system doesn't say. */
    std::int64_t inputTime = -1;
    std::int64_t outputTime = -1;

    double sampleRate = 0.0;
};

/** What a program plays and records through a stream with. */
class AudioCallback
{
public:
    virtual ~AudioCallback() = default;

    /** Before its first block, with the stream's settings as they finally are, on the stream's control thread and
        never at once with process. */
    virtual void prepare(const AudioStreamInfo& info) { (void)info; }

    /** Each block, on the audio thread, which never allocates, frees, locks, waits or calls the system. */
    virtual void process(const AudioBlock& block) noexcept = 0;

    /** After its last block, exactly once for each prepare, on the stream's control thread. A stream the system
        rebuilt, at a new rate say, stops each callback and prepares it again. */
    virtual void stopped() {}

    /** Whether the callback has nothing to play for now, as a player that is stopped; read on the control thread
        while blocks run, so from what the callback keeps atomically. A stream with Options::idleAfter stops its
        device once every callback has been idle for AudioStreamRequest::idleAfter. */
    virtual bool idle() const noexcept { return false; }
};

/** Something that happened to a device, delivered in order on the thread the program chose. */
struct AudioEvent
{
    enum class Kind
    {
        /** The stream started, or stopped as the program asked. */
        started,
        stopped,

        /** The device stopped while it ran, for loss; the stream stays stopped until started again. */
        lost,

        /** The device was opened again as it now is, after its rate, format, channels or block size changed under
            it, from before to after. */
        formatChanged,

        /** Frames lost in a direction at a position, frames -1 when the system didn't say how many; reported when the
            system said, and found by the module otherwise. */
        dropout,

        /** The device stopped calling back while it said it ran, and later started again; or called back far faster
            than its rate. */
        stalled,
        resumed,
        runaway,

        /** The callbacks took longer over a block than it lasts, nanoseconds for its frames, whether or not the system
            says sound was lost, as Windows never tells a program its output came too late; with what the program says
            made it late, from Options::describeLate. The first after a second of none is told at once, and those that
            follow within a second of it together, as one event counting them, the worst of them described. */
        late,

        /** No block came after the stream started, as when a device has no clock; the stream is stopped. */
        noSound,

        /** An input gave nothing but exact zeros for a while, as a denied or muted microphone does, and then sound
            again. */
        silentInput,
        inputSound,

        /** The system muted this program's input, as a headset's own mute button does on a Mac, so the stream's input
            is silence until it is unmuted; said when a stream with an input starts muted too. */
        inputMuted,
        inputUnmuted,

        /** A stream following the default output moved to the system's new default, deviceId; it is prepared again
            there, a new stretch. */
        moved,

        /** Every callback was idle for AudioStreamRequest::idleAfter, so the stream stopped its device, as an output not playing
            should; and one wanted sound again, so it started it again, a new stretch. */
        idle,
        active,

        /** The computer went to sleep, ending the stream's stretch, and woke after nanoseconds, the stream started
            again from new system objects. */
        sleeping,
        woke,

        /** From AudioDevices: a device came or went, or the default for a direction changed. */
        devicesChanged,
        defaultChanged,

        error
    };

    Kind kind = Kind::error;
    std::string system{};
    std::string deviceId{};
    std::string deviceName{};
    AudioDirection direction = AudioDirection::input;
    std::int64_t position = -1;
    std::int64_t frames = -1;
    bool reported = true;
    AudioLoss loss = AudioLoss::other;
    AudioStreamInfo before{};
    AudioStreamInfo after{};
    std::int64_t nanoseconds = -1;
    AudioError error{};

    /** What the program says made a late block late; empty where it says nothing. */
    std::string cause{};

    /** How many events this one stands for, when repeats were counted together. */
    int count = 1;
};

/** A stream on one device: its input, its output or both, the callbacks the program plays and records through, and
    what happens to it as events. Each stream has a control thread that owns its state and makes every call to the
    system, so a notification, the audio thread and the program's own calls never meet in the system. Its calls
    return within Options::callTimeout whatever the device does, and refuse to run on the audio thread or the
    control thread, where they would deadlock. Blocks reach the callbacks as AudioCallback says, with every lost
    frame counted in their positions; a device that changes under the stream is opened again as it now is, and one
    that is lost stays stopped and says so. */
class AudioStream
{
public:
    struct Options
    {
        /** Runs a function on the thread events are delivered on, after those posted before it; in a program with
            a message loop, ui::messageLoop::callAsync. Without it, events come on a thread of the stream's own. */
        std::function<void(std::function<void()>)> post{};

        std::function<void(const AudioEvent&)> onEvent{};

        /** How long a call waits for the control thread before saying the device isn't answering. */
        std::chrono::milliseconds callTimeout{5000};

        /** How long a started stream waits for its first block. */
        std::chrono::milliseconds firstBlockTimeout{2000};

        /** How many of its own blocks a running device may go without calling back before it is stalled. */
        double stallBlocks = 3.0;

        /** How long changes the system sends are left to settle before the device is opened again. */
        std::chrono::milliseconds settle{100};

        /** How long after the computer wakes before the stream starts again. */
        std::chrono::milliseconds wakeDelay{3000};

        /** How long an input gives exact zeros before it is said. */
        std::chrono::milliseconds silentInputAfter{10000};

        /** How often the control thread looks at a running device. */
        std::chrono::milliseconds watchEvery{20};

        /** Says what made a late block late, given when its cycle began and ended in nanoseconds on the steady clock,
            as a host's slowest plugin in that time; asked on the control thread as the block is told, its answer
            carried in the event. */
        std::function<std::string(std::int64_t began, std::int64_t ended)> describeLate{};
    };

    enum class State
    {
        stopped,
        running,
        sleeping,
        lost,

        /** The device stopped answering the control thread; the stream does nothing more. */
        failed
    };

    struct Statistics
    {
        /** Cycles and stretches since the stream was made; a stretch is one run of the system's stream. */
        std::int64_t cycles = 0;
        std::int64_t stretches = 0;

        /** Where the stretch is, in frames delivered and lost. */
        std::int64_t inputPosition = 0;
        std::int64_t outputPosition = 0;

        /** Since the stream was made. */
        std::int64_t dropouts = 0;
        std::int64_t lostFrames = 0;

        /** Blocks the callbacks took longer over than the block lasts, and the share of each block's time they
            took, smoothed. */
        std::int64_t overruns = 0;
        double load = 0.0;
    };

    AudioStream(std::shared_ptr<AudioSystem> system, std::string deviceId, AudioStreamRequest request);
    AudioStream(std::shared_ptr<AudioSystem> system, std::string deviceId, AudioStreamRequest request,
                Options options);

    /** Stops the stream, waiting at most callTimeout for a device that isn't answering, whose memory is then kept
        instead of freed under it. */
    ~AudioStream();

    AudioStream(const AudioStream&) = delete;
    AudioStream& operator=(const AudioStream&) = delete;

    /** Opens the device as asked, from new system objects, prepares the callbacks and starts it. */
    AudioError start();

    /** Stops it; each callback has had its last block and been told it stopped when this returns. */
    AudioError stop();

    /** Asks for other channels, rate or block size, opening the device again if it runs. */
    AudioError change(AudioStreamRequest request);

    /** Opens the device's own settings window while the stream runs, as an ASIO driver's control panel, returning
        without waiting while it is open. A device asking to be opened again while it is open, as a driver asks when
        its block size is changed there, is opened again once it closes, at what was chosen. Devices without one,
        as Core Audio's and Windows Audio's are, give an error. */
    AudioError showDeviceSettings();

    /** Adds a callback, prepared before its first block when the stream runs. A callback is in one stream at a
        time and lives until it is removed. */
    AudioError add(AudioCallback& callback);

    /** Takes a callback out; it has had its last block when this returns. */
    AudioError remove(AudioCallback& callback);

    State state() const noexcept;

    /** What the device ran at when it last started. */
    AudioStreamInfo info() const;

    /** What the program's threads join while the audio thread waits for their work, read when the device last started,
        so taken again after the stream is opened again. */
    AudioWorkgroup workgroup() const;

    AudioStreamRequest request() const;
    Statistics statistics() const noexcept;
    /** The device it is on, which changes for a stream following the default output. */
    std::string deviceId() const;
    AudioSystem& system() const noexcept;

    struct Impl;

private:
    // AudioDevices hears when a stream it opened closes, to let its device be opened again.
    friend class AudioDevices;
    void whenClosed(std::function<void()> closed);

    // And asks before it moves to another device, which may be refused when that one is open already.
    void whenMoving(std::function<bool(const std::string& to)> moving);

    std::shared_ptr<Impl> impl;
};

}
