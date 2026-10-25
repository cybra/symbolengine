#pragma once

#include "AudioSystem.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace symbolengine
{

/** One cycle a test has a fake device give. */
struct FakeCycle
{
    /** Its frames; 0 for the stream's block size. */
    std::size_t frames = 0;

    /** How far the device's own position jumps before the cycle, as across a gap the system saw. */
    std::int64_t jump = 0;

    /** Whether the cycle gives the device's positions; without them a gap is known only by its flag. */
    bool positions = true;

    bool inputGap = false;
    bool outputGap = false;

    /** The system says to take the input as silence; its buffers hold noise. */
    bool inputSilent = false;

    /** How far the system's clock moves with the cycle, or -1 for the cycle's own length at the device's rate. */
    std::int64_t advanceNanoseconds = -1;
};

/** How a fake device calls back on a thread of its own once its stream starts. */
struct FakeRun
{
    /** 1 for the device's own rate in real time, 0 for as fast as it can. */
    double speed = 0.0;

    /** Parts per million the device's clock runs fast, or slow when negative. */
    double ppm = 0.0;

    /** Blocks of random sizes from 0 to three times the stream's block size, where it's false they're all that. */
    bool randomSizes = false;

    /** Stops after this many cycles, still saying it runs, when not 0. */
    std::int64_t cycles = 0;
};

class FakeAudioSystem;

/** A device of a FakeAudioSystem: what it says of itself, how it misbehaves, and the cycles it gives. Its switches
    can be set while it runs. */
class FakeAudioDevice
{
public:
    /** What describe gives, apart from the switches below. Changed under the system's lock with update(). */
    void update(const std::function<void(AudioDeviceInfo&)>& change);
    AudioDeviceInfo info() const;

    /** Each input channel's sample at a frame of the device's own position. Without it, channel c's sample at
        frame n is (n + 1) mod 2^24 over 2^24, negative on odd channels, every value exact in a float. */
    std::function<float(int channel, std::int64_t frame)> input;

    std::atomic<bool> failDescribe{false};

    /** describe() waits while this is set, as a device whose calls never return. */
    std::atomic<bool> hangDescribe{false};

    std::atomic<bool> failOpen{false};
    std::atomic<bool> failStart{false};

    /** Opening it at the rate it already runs at fails, as some drivers do. */
    std::atomic<bool> failWhenSetToCurrent{false};

    /** stop() waits while this is set, as a system whose stop never returns. */
    std::atomic<bool> hangStop{false};

    /** 20 ms after stop() returns, one more cycle comes from another thread, as some systems give, before the
        system's stream object is destroyed. */
    std::atomic<bool> callAfterStop{false};

    /** start() gives a cycle before it returns, holding the lock the device's other calls take. */
    std::atomic<bool> cycleInsideStart{false};

    /** start() asks to be opened again before it returns, as an ASIO driver resetting while its buffers are made. */
    std::atomic<bool> reopenInsideStart{false};

    /** It has a settings window of its own, as an ASIO driver's control panel, which counts each time it opens. */
    std::atomic<bool> hasSettings{false};
    std::atomic<int> settingsShown{0};

    /** Gives one cycle on the calling thread, as the system's audio thread would, and moves the system's clock on.
        False when no stream on it runs. */
    bool cycle(const FakeCycle& cycle = {});

    /** Gives cycles on a thread of the device's own each time its stream starts, until it stops. */
    void run(FakeRun run);

    /** The output of the last cycle, each channel the stream took. */
    std::vector<std::vector<float>> lastOutput() const;

    /** The system says the device changed, after the change is applied, from the calling thread. */
    void changed(const std::function<void(AudioDeviceInfo&)>& change = {});

    /** The device asks its stream to open it again, as an ASIO driver asking to reset does. */
    void askToReopen();

    /** The device stops, telling its stream why. */
    void lose(AudioLoss why, std::int64_t code = 0);

    /** Whether a stream runs on it, and how many times one has been opened, started and stopped. */
    bool running() const;
    int opens() const noexcept;
    int starts() const noexcept;
    int stops() const noexcept;

    /** The request it was last opened with. */
    AudioStreamRequest lastRequest() const;

    struct State;
    explicit FakeAudioDevice(std::shared_ptr<State> state);
    ~FakeAudioDevice();

private:
    std::shared_ptr<State> state;
};

/** A scripted audio system, for tests of everything the devices module does above a system: its devices are what
    the test adds, they call back when the test says or on threads of their own, its clock moves on as they do or
    as the test moves it, and it sends the notifications a real system sends, from any thread. */
class FakeAudioSystem final : public AudioSystem
{
public:
    explicit FakeAudioSystem(std::string name = "Fake");
    ~FakeAudioSystem() override;

    /** Adds a device, telling listeners the devices changed. Its block size and rate default to 512 and 48 kHz. */
    FakeAudioDevice& add(AudioDeviceInfo info);

    /** Takes a device away, telling listeners, and its stream that it was unplugged unless quietly, as a device
        that goes quiet with no word. */
    void remove(const std::string& id, bool quietly = false);

    FakeAudioDevice* device(const std::string& id);

    void setDefault(AudioDirection direction, const std::string& id, bool tell = true);

    /** Sends a change to every listener the given number of times, from as many threads at once when threads is
        more than 1. */
    void notify(AudioSystemChange change, int times = 1, int threads = 1);

    /** The audio service restarts: every stream on the system is lost, and listeners told. */
    void restartService();

    /** Plays back what a RecordingAudioSystem wrote, at the times it wrote it divided by speed: each notice sent, each
        list of devices and the defaults put in place and told of again, as the recorder reads them only once the
        notices have settled, and each stream's losses, changes and requests to be opened again given to the stream on
        that device. Returns once the last line is played. */
    void replay(const std::vector<std::string>& lines, double speed = 1.0);

    /** The system mutes or unmutes this program's input, telling listeners. */
    void setInputMuted(bool muted);
    bool inputMuted() override;

    /** Moves the clock on. */
    void advance(std::chrono::nanoseconds by);

    /** How many calls have been made to describe devices and to open them, and how many opens set a rate or block
        size the device wasn't running at. */
    int describes() const noexcept;
    int opens() const noexcept;
    int sets() const noexcept;

    std::string name() const override;
    std::int64_t now() const noexcept override;
    std::vector<std::string> deviceIds() override;
    AudioDeviceInfo describe(const std::string& id) override;
    std::string defaultDevice(AudioDirection direction) override;
    std::unique_ptr<AudioSystemStream> open(const std::string& id, const AudioStreamRequest& request,
                                            AudioError& error) override;
    void addListener(AudioSystemListener& listener) override;
    void removeListener(AudioSystemListener& listener) override;

    struct Impl;

private:
    std::shared_ptr<Impl> impl;
};

}
