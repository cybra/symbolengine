#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace symbolengine
{

/** What a player plays: a file, an edit, a buffer. Read on the player's own thread, never the audio thread, so a
    read may take as long as it needs. */
class AudioPlayerSource
{
public:
    virtual ~AudioPlayerSource() = default;

    virtual int channels() const = 0;
    virtual double sampleRate() const = 0;

    /** Its length in frames. */
    virtual std::int64_t length() const = 0;

    /** Fills one buffer for each of its channels with frames from a frame on. */
    virtual void read(float* const* into, std::int64_t at, std::size_t frames) = 0;
};

/** Plays a source through a device's blocks: a thread of its own reads ahead from the source, at the device's rate
    when the source's is another, and the audio thread takes what it read without ever waiting on it. Starting and
    stopping fade over a time of their own whatever the block size, a jump while playing fades out and in again at its
    new place, the end comes exactly at the last frame and a loop's join is exact to the sample. When the reader falls
    behind, playback keeps time, going silent for what it lacks and moving on by as much, and says where and how
    long. A mono source plays on every output. prepare and render are the audio side, called from an AudioCallback;
    the rest are the program's, from any thread but the audio thread. */
class AudioPlayer
{
public:
    struct Event
    {
        enum class Kind
        {
            /** The reader fell behind: frames went unplayed from a position. */
            underrun,

            /** The last frame was heard. */
            finished
        };

        Kind kind = Kind::finished;
        std::int64_t position = 0;
        std::int64_t frames = 0;
    };

    struct Options
    {
        /** How far ahead the reader keeps. */
        std::chrono::milliseconds readAhead{500};

        /** How long a start, stop or jump fades over; 0 for none. */
        std::chrono::milliseconds fade{5};

        /** Where events are delivered, as for AudioStream; without post, on a thread of the player's own. */
        std::function<void(std::function<void()>)> post{};
        std::function<void(const Event&)> onEvent{};
    };

    AudioPlayer();
    explicit AudioPlayer(Options options);
    ~AudioPlayer();

    AudioPlayer(const AudioPlayer&) = delete;
    AudioPlayer& operator=(const AudioPlayer&) = delete;

    /** For a device's rate and channels, from AudioCallback::prepare, never at once with render. A player prepared
        again while it plays carries on from where it was. */
    void prepare(double sampleRate, int channels);

    /** Plays a source from a frame to another, or its end when to is -1, looping back to loopFrom at the end when it
        isn't -1; what played before fades out first. */
    void play(std::shared_ptr<AudioPlayerSource> source, std::int64_t from, std::int64_t to = -1,
              std::int64_t loopFrom = -1);

    /** Jumps to another frame of what plays, keeping its end and loop. */
    void seek(std::int64_t to);

    /** Fades out and stops; returns at once. */
    void stop();

    /** Whether it plays, until a stop's fade has ended or the last frame has been heard. */
    bool playing() const noexcept;

    /** The source's frame the next block plays from. */
    std::int64_t position() const noexcept;

    /** How many frames the reader has ready. */
    std::size_t ready() const noexcept;

    /** Writes the next frames to the outputs, on the audio thread, never allocating, locking or waiting. */
    void render(float* const* outputs, int channels, int frames) noexcept;

    struct Impl;

private:
    std::unique_ptr<Impl> impl;
};

}
