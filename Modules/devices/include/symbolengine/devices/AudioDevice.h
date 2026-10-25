#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace symbolengine
{

enum class AudioDirection
{
    input,
    output
};

/** How a device reaches the computer, as far as its system says. */
enum class AudioTransport
{
    unknown,
    builtIn,
    usb,
    firewire,
    thunderbolt,
    pci,
    bluetooth,
    hdmi,
    displayPort,
    airPlay,
    network,
    virtualDevice,
    aggregate
};

/** A device as its system lists it, read from the system's properties without opening it or setting anything. */
struct AudioDeviceInfo
{
    /** The system's own lasting identifier: Core Audio's UID, Windows Audio's endpoint ID or an ASIO driver's class
        ID. It stays the same when the device comes back, where names repeat and change. */
    std::string id{};

    std::string name{};

    /** Its place among the devices of its system with the same name, from 0, filled in by AudioDevices: how two of
        one interface are told apart when nothing else tells them. */
    int sameNameIndex = 0;

    AudioTransport transport = AudioTransport::unknown;

    /** A Bluetooth headset's microphone or output in its hands-free profile, which opening the microphone switches
        the headset to: mono, at voice quality, for its playback too. */
    bool handsFree = false;

    /** A device that hands one program's sound to another, such as a loopback driver, where most reach speakers or
        microphones. */
    bool loopback = false;

    /** Devices whose clock domains are the same and not 0 run from one clock; 0 when the system doesn't say. */
    std::uint64_t clockDomain = 0;

    /** Each channel's name as the system gives it, or its number when it gives none. */
    std::vector<std::string> inputNames{};
    std::vector<std::string> outputNames{};

    /** The rates and block sizes it offers, and those it runs at now. */
    std::vector<double> sampleRates{};
    std::vector<int> blockSizes{};
    double currentRate = 0.0;
    int currentBlock = 0;

    /** Whether it answered when it was asked about; one that didn't is listed with what went wrong. */
    bool answering = true;
    std::string problem{};

    int inputs() const noexcept { return static_cast<int>(inputNames.size()); }
    int outputs() const noexcept { return static_cast<int>(outputNames.size()); }
};

/** What a stream asks of a device. */
struct AudioStreamRequest
{
    /** The device's channels to take, by number from 0, in the order the program wants them. */
    std::vector<int> inputChannels{};
    std::vector<int> outputChannels{};

    /** The rate and block size to run at, or 0 for what the device runs at now. */
    double sampleRate = 0.0;
    int blockSize = 0;

    /** Windows Audio: exclusive mode, the device for this program alone, never shared mode in its place. */
    bool exclusive = false;

    /** Windows Audio's shared mode: how long the input's thread may be held up without losing anything, the length
        of the input's own buffer apart from the block it delivers. */
    double inputBufferSeconds = 0.2;
};

/** What a device runs at once its stream is open, as the device gives it back. */
struct AudioStreamInfo
{
    double sampleRate = 0.0;

    /** The most frames a block holds. Blocks hold any number from 1 to this. */
    int maxBlock = 0;

    int inputChannels = 0;
    int outputChannels = 0;

    /** Latency in frames, every buffer counted, or -1 when the system doesn't say. */
    std::int64_t inputLatency = -1;
    std::int64_t outputLatency = -1;

    /** What the system gave the audio thread, such as real-time priority, as it read it back. */
    std::string priority{};

    bool operator==(const AudioStreamInfo&) const = default;
};

/** What went wrong: a sentence for the user, the step that failed and the system's own code, 0 when it gave none. */
struct AudioError
{
    std::string message{};
    std::string step{};
    std::int64_t code = 0;

    explicit operator bool() const noexcept { return !message.empty(); }
};

}
