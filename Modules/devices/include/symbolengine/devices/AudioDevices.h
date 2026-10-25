#pragma once

#include "AudioDevice.h"
#include "AudioSetup.h"
#include "AudioStream.h"
#include "AudioSystem.h"

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace symbolengine
{

/** The computer's audio systems and their devices: the lists, kept up to date as devices come and go, the defaults,
    setups resolved against them, and streams opened on them, one to a device. Lists are read from each system's
    properties on a thread of their own, never opening a device, and a device that doesn't answer in time is listed
    as not answering while the rest are listed. Made and used on one thread, the program's message thread in a
    program with one; it outlives the streams it opens. */
class AudioDevices
{
public:
    struct Options
    {
        /** The systems to list; empty for this computer's own, Core Audio on a Mac and Windows Audio and ASIO on
            Windows, those built in. */
        std::vector<std::shared_ptr<AudioSystem>> systems{};

        /** Where events are delivered, as for AudioStream; the streams opened deliver theirs there too. */
        std::function<void(std::function<void()>)> post{};
        std::function<void(const AudioEvent&)> onEvent{};

        /** How long a device has to answer when it is listed. */
        std::chrono::milliseconds describeTimeout{2000};

        /** How long changes are left to settle before the lists are read again. */
        std::chrono::milliseconds settle{100};

        /** For the streams opened; their post and onEvent are these. */
        AudioStream::Options streams{};
    };

    AudioDevices();
    explicit AudioDevices(Options options);
    ~AudioDevices();

    AudioDevices(const AudioDevices&) = delete;
    AudioDevices& operator=(const AudioDevices&) = delete;

    /** The systems' names, in the order given. */
    std::vector<std::string> systems() const;

    /** A system's devices as last listed, each with its place among those of the same name. */
    std::vector<AudioDeviceInfo> devices(const std::string& system) const;

    /** A system's default device for a direction, as last listed, or empty. */
    std::string defaultDevice(const std::string& system, AudioDirection direction) const;

    /** Resolves a setup against its system's devices as last listed. */
    AudioResolution resolve(const AudioSetup& setup) const;

    /** Opens a stream on a device, not yet started; one stream to a device, a second refused. */
    std::unique_ptr<AudioStream> open(const std::string& system, const std::string& deviceId,
                                      AudioStreamRequest request, AudioError& error);

    struct Impl;

private:
    std::shared_ptr<Impl> impl;
};

}
