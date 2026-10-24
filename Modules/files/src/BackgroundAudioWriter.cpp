#include "symbolengine/files/BackgroundAudioWriter.h"

#include "Samples.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

// The samples wait in a ring, one for each channel, that the thread adding them and the writer's thread each go round
// in turn: added counts every frame put in, taken every frame written, and neither thread waits on the other.
namespace symbolengine
{

struct BackgroundAudioWriter::State
{
    std::unique_ptr<AudioFileWriter> file;
    std::size_t capacity = 0;
    std::chrono::milliseconds flushEvery{};
    Written written;

    // The bits a whole-number format holds its samples in, to give them as they read back, or 0 to give them as they
    // were written.
    int wholeBits = 0;

    std::vector<std::vector<float>> ring;
    std::atomic<std::uint64_t> added{0}, taken{0};
    std::atomic<bool> failure{false};
    std::error_code firstError{};

    std::mutex mutex;
    std::condition_variable wake;
    bool stopping = false;
    std::thread thread;

    void fail(const std::error_code& error)
    {
        firstError = error;
        failure = true;
    }

    // Writes one stretch of the ring, from frame at of each channel, and gives it to written.
    void writeStretch(std::size_t at, std::size_t frames, std::vector<const float*>& pointers,
                      std::vector<std::vector<float>>& readBack)
    {
        for (std::size_t c = 0; c < ring.size(); ++c)
            pointers[c] = ring[c].data() + at;

        std::error_code error;
        file->write(pointers.data(), frames, error);
        if (error)
        {
            fail(error);
            return;
        }

        if (!written)
            return;

        if (wholeBits > 0)
            for (std::size_t c = 0; c < ring.size(); ++c)
            {
                readBack[c].resize(std::max(readBack[c].size(), frames));
                for (std::size_t i = 0; i < frames; ++i)
                    readBack[c][i] = detail::readsBackAs(pointers[c][i], wholeBits);
                pointers[c] = readBack[c].data();
            }

        written(pointers.data(), frames);
    }

    void run()
    {
        using clock = std::chrono::steady_clock;

        // With nothing to write, the thread looks again this often; adding samples never wakes it, so the thread
        // adding them never touches a lock.
        constexpr auto rest = std::chrono::milliseconds(10);

        std::vector<const float*> pointers(ring.size());
        std::vector<std::vector<float>> readBack(ring.size());
        auto flushedAt = clock::now();
        std::uint64_t flushedFrames = 0;

        for (;;)
        {
            bool last = false;
            {
                std::unique_lock lock(mutex);
                wake.wait_for(lock, rest,
                              [this] { return stopping || added.load(std::memory_order_acquire) != taken.load(); });
                last = stopping;
            }

            // Everything waiting, in two stretches when it goes round the end of the ring.
            const auto end = added.load(std::memory_order_acquire);
            for (auto start = taken.load(); start < end;)
            {
                const auto at = static_cast<std::size_t>(start % capacity);
                const auto frames = static_cast<std::size_t>(std::min<std::uint64_t>(end - start, capacity - at));
                if (!failure)
                    writeStretch(at, frames, pointers, readBack);
                start += frames;
                taken.store(start, std::memory_order_release);
            }

            if (last)
                return;

            if (!failure && clock::now() - flushedAt >= flushEvery)
            {
                if (file->frames() != flushedFrames)
                {
                    std::error_code error;
                    file->flush(error);
                    if (error)
                        fail(error);
                    flushedFrames = file->frames();
                }
                flushedAt = clock::now();
            }
        }
    }
};

BackgroundAudioWriter::BackgroundAudioWriter(std::unique_ptr<AudioFileWriter> writer, std::size_t bufferFrames,
                                             std::chrono::milliseconds flushEvery, Written written)
    : state(std::make_unique<State>())
{
    auto& s = *state;
    const auto& settings = writer->settings();
    const auto whole = settings.format == "WAV" || settings.format == "AIFF" || settings.format == "FLAC";
    s.wholeBits = whole && !settings.floatingPoint ? settings.bits : 0;
    s.capacity = std::max<std::size_t>(bufferFrames, 1);
    s.ring.assign(static_cast<std::size_t>(std::max(settings.channels, 0)), std::vector<float>(s.capacity));
    s.file = std::move(writer);
    s.flushEvery = flushEvery;
    s.written = std::move(written);
    s.thread = std::thread([&s] { s.run(); });
}

BackgroundAudioWriter::~BackgroundAudioWriter()
{
    std::error_code ignored;
    finish(ignored);
}

bool BackgroundAudioWriter::write(const float* const* channels, std::size_t frames) noexcept
{
    auto& s = *state;
    if (s.failure.load(std::memory_order_relaxed))
        return false;

    const auto added = s.added.load(std::memory_order_relaxed);
    const auto waiting = added - s.taken.load(std::memory_order_acquire);
    if (frames > s.capacity - waiting)
        return false;

    const auto at = static_cast<std::size_t>(added % s.capacity);
    const auto first = std::min(frames, s.capacity - at);
    for (std::size_t c = 0; c < s.ring.size(); ++c)
    {
        std::copy(channels[c], channels[c] + first, s.ring[c].data() + at);
        std::copy(channels[c] + first, channels[c] + frames, s.ring[c].data());
    }

    s.added.store(added + frames, std::memory_order_release);
    return true;
}

bool BackgroundAudioWriter::failed() const noexcept
{
    return state->failure.load();
}

void BackgroundAudioWriter::finish(std::error_code& error)
{
    error.clear();
    auto& s = *state;
    if (!s.thread.joinable())
        return;

    {
        std::lock_guard lock(s.mutex);
        s.stopping = true;
    }
    s.wake.notify_one();
    s.thread.join();

    s.file->finish(error);
    if (s.failure)
        error = s.firstError;
}

void BackgroundAudioWriter::finish()
{
    std::error_code error;
    finish(error);
    if (error)
        throw std::filesystem::filesystem_error("can't write the audio file", error);
}

}
