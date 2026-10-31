#pragma once

#include "AudioFile.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <system_error>

namespace symbolengine
{

/** An audio file written from a thread that mustn't wait, such as an audio device's. write() copies samples into a
    buffer and returns at once, never locking, waiting or allocating, and a thread of the writer's own writes them to
    the file, flushing it every interval, so a WAV or AIFF recording stopped by a crash reads to within an interval of
    where it stopped. */
class BackgroundAudioWriter
{
public:
    /** Given each stretch on the writer's thread once it is in the file, one buffer per channel, with its samples as
        the file holds them: a WAV's, AIFF's or FLAC's whole numbers as they read back, and floats as they are. Ogg
        Vorbis and Opus code their samples, and give them as they were written. */
    using Written = std::function<void(const float* const* channels, std::size_t frames)>;

    /** Takes the writer, with room for bufferFrames samples of each channel waiting to be written, and starts its
        thread, which flushes the file every flushEvery. Given no writer, as createAudio() can return, it is failed from
        the start. A function given as written that throws fails it as writing the file does. */
    BackgroundAudioWriter(std::unique_ptr<AudioFileWriter> writer, std::size_t bufferFrames,
                          std::chrono::milliseconds flushEvery = std::chrono::seconds(1), Written written = {});

    /** Finishes as finish() does, ignoring anything that goes wrong. */
    ~BackgroundAudioWriter();

    BackgroundAudioWriter(const BackgroundAudioWriter&) = delete;
    BackgroundAudioWriter& operator=(const BackgroundAudioWriter&) = delete;

    /** Adds frames samples of each of the file's channels, from one buffer per channel, to those waiting. Returns
        false, adding none of them, when there isn't room for them all, or once writing the file has gone wrong. One
        thread at a time adds samples, and none once finish() has begun. */
    bool write(const float* const* channels, std::size_t frames) noexcept;

    /** Whether writing the file has gone wrong; nothing more is written to it once it has. */
    bool failed() const noexcept;

    /** From any thread: the samples of each channel in the file so far, and those write() took that never reached it,
        as writing it went wrong with them waiting. Once finished, every frame write() took is one or the other. */
    std::uint64_t framesWritten() const noexcept;
    std::uint64_t framesLost() const noexcept;

    /** Writes everything still waiting, finishes the file and stops the thread. Throws
        std::filesystem::filesystem_error for the first thing that went wrong in writing the file, or, given a
        std::error_code, sets it instead. Finishing again does nothing. */
    void finish();
    void finish(std::error_code& error);

private:
    struct State;
    std::unique_ptr<State> state;
};

}
