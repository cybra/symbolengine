#pragma once

#include "AudioDevice.h"

#include <symbolengine/core/Value.h>

#include <string>
#include <vector>

namespace symbolengine
{

/** What a device is found as when a setup is resolved against the devices there are. */
struct AudioResolution
{
    struct Direction
    {
        /** The device to open, or empty for none. */
        std::string id{};
        std::string name{};

        /** The device chosen, or the system's default when that is what was chosen; false when another was put in
            its place, which said tells. */
        bool exact = false;

        /** Found by its name, its identifier having changed. */
        bool byName = false;

        /** The devices sharing the saved name when the identifier found none, for the program to ask between;
            nothing is opened for this direction while there are. */
        std::vector<std::string> ambiguous{};

        /** The device's channels to take; those saved that the device no longer has are left out, and said. */
        std::vector<int> channels{};
        bool channelsMissing = false;

        /** The rate and block size to open it at, 0 for what it runs at now, and what to tell the user of what was
            put in place of what was saved. */
        double sampleRate = 0.0;
        int blockSize = 0;

        std::string said{};
    };

    Direction input{};
    Direction output{};
};

/** The devices a user chose, kept between launches and found again with resolve(). Each direction is kept apart: a
    device chosen, the system's default followed, or none, which stays none. */
struct AudioSetup
{
    enum class Choice
    {
        none,
        device,
        systemDefault
    };

    struct Direction
    {
        Choice choice = Choice::systemDefault;

        /** The device chosen, as AudioDeviceInfo has it. */
        std::string id{};
        std::string name{};
        int sameNameIndex = 0;

        /** The device's channels, in order; none for its first one, or for an output its first two. */
        std::vector<int> channels{};

        /** The devices the user has chosen, the latest first, up to eight: a headset's hands-free microphone or a
            loopback device is put in another's place only when the user chose it before. */
        std::vector<std::string> chosenBefore{};
    };

    /** The system the devices are on, Core Audio, Windows Audio or ASIO. */
    std::string system{};

    Direction input{Choice::none};
    Direction output{};

    /** The rate and block size the user chose, or 0 for what each device runs at. */
    double sampleRate = 0.0;
    int blockSize = 0;

    /** Windows Audio's exclusive mode. */
    bool exclusive = false;

    /** Chooses a device for a direction. */
    void choose(AudioDirection direction, const AudioDeviceInfo& device, std::vector<int> channels = {});

    /** Finds the setup's devices among those given, with the system's defaults:
        - each direction by the device's identifier first, then by a name only one device has, asking between those
          that share it;
        - a device missing put in place direction by direction and setting by setting, so a missing input leaves the
          output as it was: the system's default first, then the first device with channels that way, never a
          headset's hands-free microphone or a loopback device the user hasn't chosen before;
        - a rate or block size the device offers restored exactly, and otherwise the nearest rate, the higher of two
          as near, and the next larger block size, or the largest; every substitution said. */
    AudioResolution resolve(const std::vector<AudioDeviceInfo>& devices, const std::string& defaultInput,
                            const std::string& defaultOutput) const;

    /** As JSON for a settings file, and back; anything missing or of the wrong kind reads as its default. */
    Value toValue() const;
    static AudioSetup fromValue(const Value& value);
};

}
