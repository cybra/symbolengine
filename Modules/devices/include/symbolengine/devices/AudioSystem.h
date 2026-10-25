#pragma once

#include "AudioDevice.h"
#include "AudioWorkgroup.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace symbolengine
{

/** Why a device stopped while it ran. */
enum class AudioLoss
{
    unplugged,

    /** Another program took it for its own exclusive use. */
    takenExclusively,

    /** It stopped answering, or stopped calling back while it said it ran. */
    notAnswering,

    /** The system's audio service restarted, and every device with it. */
    serviceRestarted,

    /** The system refused it, as for a microphone the program may not use. */
    refused,

    /** It lost the clock it runs from, as an interface following another's clock does when that is unplugged. */
    clockLost,

    other
};

/** What a system tells the devices module of, from any thread. */
enum class AudioSystemChange
{
    /** A device came or went, or a device's description changed. */
    devices,

    /** A default device changed. The system sends this for each role and direction, sometimes more than once for
        one change, so it is a hint to ask again. */
    defaults,

    /** The system's audio service restarted: every stream open on it is gone. */
    serviceRestarted,

    /** The computer is going to sleep, or has woken. */
    sleeping,
    woke,

    /** The system muted or unmuted this program's input, as a headset's own mute button does on a Mac, after which
        every input gives silence until it is unmuted; inputMuted() says which. */
    inputMute
};

/** One cycle of a device, as its system's audio thread gives it. */
struct AudioSystemCycle
{
    /** One buffer for each channel the stream took, in its order; an input buffer may be null for silence, and the
        output buffers may hold anything. */
    const float* const* inputs = nullptr;
    float* const* outputs = nullptr;
    int inputChannels = 0;
    int outputChannels = 0;
    std::size_t frames = 0;

    /** The system's own count of the cycle's first input and output frames, which a gap jumps, or -1 when the system
        gives none: Windows Audio's device position, Core Audio's sample time, ASIO's sample position. */
    std::int64_t inputPosition = -1;
    std::int64_t outputPosition = -1;

    /** When the cycle's input was captured and when its output will be heard, in nanoseconds on the system's steady
        clock, or -1. */
    std::int64_t inputTime = -1;
    std::int64_t outputTime = -1;

    /** The system says something was lost before this cycle: Windows Audio's discontinuity, Core Audio's overload,
        ASIO's resync. Where the positions are given, they say how much. */
    bool inputGap = false;
    bool outputGap = false;

    /** The system says to take the input as silence, whatever its buffers hold. */
    bool inputSilent = false;
};

/** What a system stream calls with what happens to it. The devices module is the client. */
class AudioSystemClient
{
public:
    /** Each cycle, on the system's audio thread. */
    virtual void cycle(const AudioSystemCycle& cycle) noexcept = 0;

    /** The device's rate, format, channels, block size or clock changed, from any thread. */
    virtual void changed() noexcept = 0;

    /** The device asks to be opened again, whether anything about it changed or not, as an ASIO driver asking to
        reset does, from any thread. It is opened again as it now runs once changes have settled. */
    virtual void reopen() noexcept = 0;

    /** The device stopped while it ran, from any thread, the audio thread included. */
    virtual void lost(AudioLoss why, std::int64_t code) noexcept = 0;

    virtual ~AudioSystemClient() = default;
};

/** A stream open on one device. Made by AudioSystem::open, used from the devices module's control thread alone. */
class AudioSystemStream
{
public:
    virtual ~AudioSystemStream() = default;

    /** What the device runs at, as it gives it back now. */
    virtual AudioStreamInfo info() const = 0;

    /** Starts its cycles, which go to the client on the system's audio thread, possibly before this returns. False,
        with the error set, when it can't. */
    virtual bool start(AudioSystemClient& client, AudioError& error) = 0;

    /** Stops its cycles, waiting a bounded time; the devices module stops listening to it first, so a cycle the
        system gives after this returns does nothing. */
    virtual void stop() noexcept = 0;

    /** Opens the device's own settings window, as an ASIO driver's control panel, returning once it is asked for,
        never waiting while it is open; what is changed there reaches the client as the device says it. A device
        with none gives an error. */
    virtual AudioError showSettings() { return {"The audio device has no settings window of its own.", "settings"}; }

    /** What the program's own threads join to work to the device's deadline, read again each time it starts. */
    virtual AudioWorkgroup workgroup() const { return {}; }
};

/** What a system tells of its changes to. */
class AudioSystemListener
{
public:
    virtual void systemChanged(AudioSystemChange change) noexcept = 0;

    virtual ~AudioSystemListener() = default;
};

/** One of the computer's audio systems, behind the narrow layer the devices module is written over: Core Audio,
    Windows Audio, ASIO, or FakeAudioSystem in tests. Everything above it, the lists, choosing and restoring, each
    device's control and the timeline, is written once over this. The devices module calls a system only from threads
    of its own, never a program's, so a system can set those threads up as it needs, as Windows' COM asks. */
class AudioSystem
{
public:
    virtual ~AudioSystem() = default;

    /** Its name as users know it: Core Audio, Windows Audio, ASIO. */
    virtual std::string name() const = 0;

    /** The steady clock its host times are on, in nanoseconds. */
    virtual std::int64_t now() const noexcept = 0;

    /** Its devices' identifiers, from its own list, opening nothing. */
    virtual std::vector<std::string> deviceIds() = 0;

    /** A device's description, from its properties, opening and setting nothing. A device that doesn't answer gives
        answering false and a problem. */
    virtual AudioDeviceInfo describe(const std::string& id) = 0;

    /** The default device for a direction, or empty when there is none. */
    virtual std::string defaultDevice(AudioDirection direction) = 0;

    /** Whether the system has muted this program's input, as Core Audio does from macOS 14, so every input gives
        silence; false for a system that never does. */
    virtual bool inputMuted() { return false; }

    /** Opens a stream on a device, without starting it, or gives nothing with the error set. */
    virtual std::unique_ptr<AudioSystemStream> open(const std::string& id, const AudioStreamRequest& request,
                                                    AudioError& error) = 0;

    /** Tells a listener of changes from now on, from any thread, until it is removed; removing it returns once no
        call to it is under way. */
    virtual void addListener(AudioSystemListener& listener) = 0;
    virtual void removeListener(AudioSystemListener& listener) = 0;
};

}
