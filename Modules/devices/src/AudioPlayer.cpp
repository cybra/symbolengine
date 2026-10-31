#include "symbolengine/devices/AudioPlayer.h"

#include "EventQueue.h"

#include <symbolengine/core/LockFreeFifo.h>
#include <symbolengine/core/Resampler.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

// The reader fills one ring of frames for each output channel, and a ring of chunk headers saying what each stretch
// of frames is: which play it belongs to, where it falls on that play's timeline of device frames, and the source
// frame it starts at. The audio thread takes chunks in order, passing over those of plays it no longer wants and any
// that came too late for their place on the timeline, so frames it went without are never played late: the reader is
// told how many it missed and moves on by as much.
namespace symbolengine
{

namespace
{
using namespace std::chrono_literals;

constexpr std::size_t chunkFrames = 512;

struct Chunk
{
    std::uint64_t generation = 0;
    std::int64_t index = 0;      // where it falls on its play's timeline, in device frames
    std::int64_t position = 0;   // the source frame it starts at
    double perFrame = 1.0;       // source frames a device frame
    std::int64_t loopFrom = -1;  // where its play loops back to from loopEnd, or -1
    std::int64_t loopEnd = 0;
    std::uint32_t frames = 0;
    bool end = false;

    std::int64_t sourceAt(std::uint32_t used) const noexcept
    {
        auto at = position + static_cast<std::int64_t>(std::llround(used * perFrame));
        if (loopFrom >= 0 && at >= loopEnd && loopEnd > loopFrom)
            at = loopFrom + (at - loopFrom) % (loopEnd - loopFrom);
        return at;
    }
};

struct Note
{
    AudioPlayer::Event::Kind kind = AudioPlayer::Event::Kind::underrun;
    std::int64_t position = 0;
    std::int64_t frames = 0;
};

struct Request
{
    std::uint64_t generation = 0;
    std::shared_ptr<AudioPlayerSource> source{};
    std::int64_t from = 0;
    std::int64_t to = -1;
    std::int64_t loopFrom = -1;
};
}

struct AudioPlayer::Impl
{
    Options options;
    std::shared_ptr<devices::detail::EventQueue> queue;

    // The program's side.
    std::mutex lock;
    std::condition_variable wake;
    Request request;
    std::uint64_t generations = 0;
    bool quitting = false;
    std::thread reader;

    // Made by prepare, which the reader is kept out of while it runs.
    std::mutex fillLock;
    double rate = 48000.0;
    int channels = 2;
    double fadeStep = 1.0;
    std::vector<std::unique_ptr<LockFreeFifo<float>>> rings;
    std::unique_ptr<LockFreeFifo<Chunk>> chunks;

    // Between the threads.
    std::atomic<std::uint64_t> asked{0};
    std::atomic<std::uint64_t> wanted{0};
    std::atomic<std::uint64_t> heard{0};
    std::atomic<std::uint64_t> finishedGeneration{0};
    std::atomic<std::int64_t> heardPosition{0};
    std::atomic<std::int64_t> playedIndex{0};
    std::atomic<bool> sounding{false};
    // Frames whose chunks are given and not yet played or passed over: a ring holds a chunk's frames a moment before
    // its header says what they are, and the audio thread can play them only once it does.
    std::atomic<std::int64_t> playable{0};
    LockFreeFifo<Note> notes{256};

    // The audio thread's own.
    std::uint64_t current = 0;
    Chunk chunk{};
    std::uint32_t used = 0;
    bool holding = false;  // a chunk taken and not yet played through
    std::int64_t played = 0;
    double gain = 0.0;
    std::int64_t lackingFrom = -1, lacking = 0;

    // The reader's own.
    struct Fill
    {
        bool readFailed = false;
        std::uint64_t generation = 0;
        std::shared_ptr<AudioPlayerSource> source;
        int sourceChannels = 0;
        std::int64_t from = 0, end = 0, loopFrom = -1, cursor = 0;
        double ratio = 1.0;
        std::unique_ptr<Resampler> resampler;
        std::int64_t index = 0, total = -1;
        std::size_t pendingAt = 0, pending = 0;
        bool flushing = false, ended = false;
    } fill;
    std::vector<std::vector<float>> sourceBuffers, deviceBuffers;
    std::vector<float*> sourcePointers, devicePointers;
    std::vector<float> zeros;

    // The program's calls

    void start()
    {
        reader = std::thread([this] { readLoop(); });
    }

    void ask(Request next, bool keepSource)
    {
        std::uint64_t generation = 0;
        bool playing = false;
        {
            const std::lock_guard hold(lock);
            if (keepSource)
            {
                if (request.source == nullptr)
                    return;
                next.source = request.source;
                next.to = request.to;
                next.loopFrom = request.loopFrom;
            }
            next.generation = ++generations;
            generation = next.generation;
            request = std::move(next);
            playing = request.source != nullptr;
            if (playing)
                heardPosition = request.from;
            asked = generation;
            wanted = playing ? generation : 0;
        }
        wake.notify_one();
    }

    // The audio thread

    void render(float* const* outputs, int count, int frames) noexcept
    {
        for (int c = 0; c < count; ++c)
            std::fill(outputs[c], outputs[c] + frames, 0.0f);
        if (rings.empty() || frames <= 0)
            return;

        auto want = wanted.load();
        int done = 0;
        while (done < frames)
        {
            // Idle: the wanted play's first chunk, past those of plays before it.
            if (current == 0)
            {
                if (want == 0 || want == finishedGeneration.load())
                    break;
                if (!holding && !takeChunk())
                    break;
                if (chunk.generation != want)
                {
                    passChunk();
                    continue;
                }
                // Its place on the timeline is told before the play it belongs to, so the reader, seeing the play,
                // sees where it is and never follows the play before's place.
                current = want;
                played = chunk.index;
                playedIndex = played;
                heard = want;
                gain = 0.0;
                sounding = true;
            }

            // Playing: what is wanted now fades this play out, and once silent it is let go.
            bool leaving = want != current;
            if (leaving && gain == 0.0)
            {
                leave(want);
                continue;
            }
            if (!holding && !takeChunk())
            {
                if (leaving)
                {
                    leave(want);
                    continue;
                }
                // Nothing read in time: silence, and the reader moves on by as much.
                const auto short_ = frames - done;
                if (lackingFrom < 0)
                    lackingFrom = heardPosition.load();
                lacking += short_;
                played += short_;
                playedIndex = played;
                heardPosition += static_cast<std::int64_t>(std::llround(short_ * chunk.perFrame));
                gain = 0.0;
                break;
            }
            if (chunk.generation < current)
            {
                passChunk();
                continue;
            }
            if (chunk.generation > current)
            {
                // The reader has moved on to a later play, so this one has nothing more; the chunk is kept for it.
                want = wanted.load();
                leave(want, true);
                continue;
            }
            if (chunk.end)
            {
                finish();
                continue;
            }
            // Frames that came too late for their place on the timeline are passed over.
            if (chunk.index + used < played)
                passFrames(static_cast<std::uint32_t>(played - chunk.index - used));
            if (!holding)
                continue;
            // A chunk from further on waits in silence for its place.
            if (chunk.index + used > played)
            {
                const auto wait = std::min<std::int64_t>(frames - done, chunk.index + used - played);
                if (lackingFrom < 0)
                    lackingFrom = heardPosition.load();
                lacking += wait;
                played += wait;
                done += static_cast<int>(wait);
                gain = 0.0;
                continue;
            }
            tellLacking();

            const auto n = std::min<std::int64_t>(frames - done, chunk.frames - used);
            for (int c = 0; c < static_cast<int>(rings.size()); ++c)
            {
                if (c < count)
                    rings[static_cast<std::size_t>(c)]->read(outputs[c] + done, static_cast<std::size_t>(n));
                else
                    rings[static_cast<std::size_t>(c)]->skip(static_cast<std::size_t>(n));
            }
            playable -= n;
            const double target = leaving ? 0.0 : 1.0;
            for (std::int64_t i = 0; i < n; ++i)
            {
                gain = gain < target ? std::min(target, gain + fadeStep) : std::max(target, gain - fadeStep);
                for (int c = 0; c < count; ++c)
                    outputs[c][done + i] *= static_cast<float>(gain);
            }
            used += static_cast<std::uint32_t>(n);
            done += static_cast<int>(n);
            played += n;
            heardPosition = chunk.sourceAt(used);
            if (used == chunk.frames)
                holding = false;
            if (leaving && gain == 0.0)
                leave(want);
        }
        if (current != 0)
            playedIndex = played;
    }

    bool takeChunk() noexcept
    {
        if (!chunks->pop(chunk))
            return false;
        used = 0;
        holding = true;
        return true;
    }

    void passFrames(std::uint32_t frames) noexcept
    {
        const auto count = std::min(frames, chunk.frames - used);
        for (auto& ring : rings)
            ring->skip(count);
        playable -= count;
        used += count;
        if (used == chunk.frames)
            holding = false;
    }

    void passChunk() noexcept
    {
        passFrames(chunk.frames - used);
        holding = false;
    }

    // Lets go of the play heard; a chunk in hand of a later play is kept for it.
    void leave(std::uint64_t want, bool keepChunk = false) noexcept
    {
        if (holding && !keepChunk)
            passChunk();
        current = 0;
        gain = 0.0;
        tellLacking();
        sounding = want != 0;
    }

    void finish() noexcept
    {
        holding = false;
        tellLacking();
        finishedGeneration = current;
        current = 0;
        sounding = false;
        notes.push({AudioPlayer::Event::Kind::finished, heardPosition.load(), 0});
    }

    void tellLacking() noexcept
    {
        if (lacking > 0)
            notes.push({AudioPlayer::Event::Kind::underrun, lackingFrom, lacking});
        lacking = 0;
        lackingFrom = -1;
    }

    // The reader

    void readLoop()
    {
        std::unique_lock hold(lock);
        while (!quitting)
        {
            wake.wait_for(hold, 5ms);
            if (quitting)
                break;
            const auto now = request;
            hold.unlock();
            {
                const std::lock_guard filling(fillLock);
                if (chunks != nullptr)
                {
                    if (now.generation != fill.generation)
                        begin(now);
                    if (fill.source != nullptr && !fill.ended)
                        fillSome();
                }
            }
            tellEvents();
            hold.lock();
        }
    }

    void begin(const Request& now)
    {
        fill = {};
        fill.generation = now.generation;
        if (now.source == nullptr)
            return;
        fill.source = now.source;
        fill.sourceChannels = std::max(1, now.source->channels());
        const auto length = now.source->length();
        fill.end = now.to < 0 ? length : std::min(now.to, length);
        fill.loopFrom = now.loopFrom >= 0 && now.loopFrom < fill.end ? now.loopFrom : -1;
        fill.from = std::clamp<std::int64_t>(now.from, 0, fill.end);
        fill.cursor = fill.from;
        fill.ratio = rate / now.source->sampleRate();
        if (std::abs(fill.ratio - 1.0) > 1e-9)
            fill.resampler = std::make_unique<Resampler>(fill.sourceChannels, fill.ratio, 1024);
        fill.total = fill.loopFrom >= 0 ? -1
                                        : static_cast<std::int64_t>(
                                              std::llround(static_cast<double>(fill.end - fill.from) * fill.ratio));

        sourceBuffers.assign(static_cast<std::size_t>(fill.sourceChannels), std::vector<float>(1024 + chunkFrames));
        deviceBuffers.assign(static_cast<std::size_t>(fill.sourceChannels), std::vector<float>(chunkFrames));
        sourcePointers.clear();
        devicePointers.clear();
        for (auto& buffer : sourceBuffers)
            sourcePointers.push_back(buffer.data());
        for (auto& buffer : deviceBuffers)
            devicePointers.push_back(buffer.data());
        zeros.assign(1024 + chunkFrames, 0.0f);
    }

    // The source's frame at a place on the play's timeline.
    std::int64_t sourceAt(std::int64_t index) const
    {
        auto at = static_cast<double>(fill.from) + static_cast<double>(index) / fill.ratio;
        if (fill.loopFrom >= 0 && at >= static_cast<double>(fill.end))
            at = static_cast<double>(fill.loopFrom) +
                 std::fmod(at - static_cast<double>(fill.loopFrom), static_cast<double>(fill.end - fill.loopFrom));
        return static_cast<std::int64_t>(std::llround(at));
    }

    bool room() const
    {
        return rings.front()->writable() >= chunkFrames && chunks->writable() >= 2;
    }

    void fillSome()
    {
        // Frames the audio thread went without are passed over here too, so what it plays next is on time.
        if (heard.load() == fill.generation)
            if (const auto at = playedIndex.load(); at > fill.index)
            {
                fill.index = at;
                fill.cursor = sourceAt(fill.index);
                if (fill.resampler != nullptr)
                    fill.resampler->reset();
                fill.pending = 0;
                if (fill.total >= 0 && fill.index >= fill.total)
                {
                    endPlay();
                    return;
                }
            }

        // A ring's worth at most, and none once another play is asked for, so the reader comes back to look.
        const auto most = rings.front()->capacity();
        for (std::size_t given = 0; !fill.ended && room() && asked.load() == fill.generation && given < most;
             given += chunkFrames)
            if (fill.resampler == nullptr)
                readPlain();
            else
                readResampled();
    }

    void readPlain()
    {
        // A chunk's worth, round a loop as many times as it takes.
        const auto position = fill.cursor;
        std::size_t filled = 0;
        while (filled < chunkFrames)
        {
            if (fill.cursor >= fill.end)
            {
                if (fill.loopFrom < 0)
                    break;
                fill.cursor = fill.loopFrom;
            }
            const auto frames =
                static_cast<std::size_t>(std::min<std::int64_t>(static_cast<std::int64_t>(chunkFrames - filled), fill.end - fill.cursor));
            std::vector<float*> into(sourcePointers.begin(), sourcePointers.end());
            for (auto& pointer : into)
                pointer += filled;
            readSource(into.data(), fill.cursor, frames);
            filled += frames;
            fill.cursor += static_cast<std::int64_t>(frames);
        }
        if (filled > 0)
            give(sourcePointers.data(), filled, position, 1.0);
        if (fill.cursor >= fill.end && fill.loopFrom < 0)
            endPlay();
    }

    void readResampled()
    {
        std::size_t produced = 0;
        const auto position = sourceAt(fill.index);
        while (produced < chunkFrames)
        {
            if (fill.pending == 0)
            {
                if (fill.cursor >= fill.end && fill.loopFrom >= 0)
                    fill.cursor = fill.loopFrom;
                if (fill.cursor >= fill.end)
                {
                    // Past the end, silence carries the last frames out of the resampler.
                    fill.flushing = true;
                    for (auto& buffer : sourceBuffers)
                        std::fill(buffer.begin(), buffer.end(), 0.0f);
                    fill.pending = 1024;
                }
                else
                {
                    fill.pending = static_cast<std::size_t>(std::min<std::int64_t>(1024, fill.end - fill.cursor));
                    readSource(sourcePointers.data(), fill.cursor, fill.pending);
                    fill.cursor += static_cast<std::int64_t>(fill.pending);
                }
                fill.pendingAt = 0;
            }
            std::vector<const float*> in(sourcePointers.begin(), sourcePointers.end());
            for (auto& pointer : in)
                pointer += fill.pendingAt;
            std::vector<float*> out(devicePointers.begin(), devicePointers.end());
            for (auto& pointer : out)
                pointer += produced;
            const auto done = fill.resampler->process(in.data(), fill.pending, out.data(), chunkFrames - produced);
            fill.pending -= done.consumed;
            fill.pendingAt += done.consumed;
            produced += done.produced;
            if (fill.total >= 0 && fill.index + static_cast<std::int64_t>(produced) >= fill.total)
            {
                produced = static_cast<std::size_t>(fill.total - fill.index);
                break;
            }
        }
        if (produced > 0)
            give(devicePointers.data(), produced, position, 1.0 / fill.ratio);
        if (fill.total >= 0 && fill.index >= fill.total)
            endPlay();
    }

    // Writes frames of the source's channels to the device's rings, a mono source to every one, and its header.
    void give(float* const* from, std::size_t frames, std::int64_t position, double perFrame)
    {
        for (int c = 0; c < channels; ++c)
        {
            const float* channel = fill.sourceChannels == 1     ? from[0]
                                   : c < fill.sourceChannels    ? from[c]
                                                                : zeros.data();
            rings[static_cast<std::size_t>(c)]->write(channel, frames);
        }
        chunks->push({fill.generation, fill.index, position, perFrame, fill.loopFrom, fill.end,
                      static_cast<std::uint32_t>(frames), false});
        playable += static_cast<std::int64_t>(frames);
        fill.index += static_cast<std::int64_t>(frames);
    }

    void endPlay()
    {
        chunks->push({fill.generation, fill.index, fill.end, 1.0, -1, 0, 0, true});
        fill.ended = true;
    }

    void tellEvents()
    {
        Note note;
        while (notes.pop(note))
            tell({note.kind, note.position, note.frames});
    }

    void tell(const Event& event)
    {
        if (!options.onEvent)
            return;
        auto deliver = [handler = options.onEvent, event] { handler(event); };
        if (options.post)
            options.post(std::move(deliver));
        else
            queue->post(std::move(deliver));
    }

    // A source that can't be read leaves silence where it was to play, the program told, rather than an exception
    // ending the program from this thread.
    void readSource(float* const* into, std::int64_t at, std::size_t frames)
    {
        try
        {
            fill.source->read(into, at, frames);
            return;
        }
        catch (...)
        {
        }
        for (int c = 0; c < fill.sourceChannels; ++c)
            std::fill_n(into[c], frames, 0.0f);
        if (!fill.readFailed)
        {
            fill.readFailed = true;
            tell({AudioPlayer::Event::Kind::readFailed, at, static_cast<std::int64_t>(frames)});
        }
    }
};

AudioPlayer::AudioPlayer() : AudioPlayer(Options{}) {}

AudioPlayer::AudioPlayer(Options options) : impl(std::make_unique<Impl>())
{
    impl->options = std::move(options);
    if (!impl->options.post && impl->options.onEvent)
        impl->queue = devices::detail::EventQueue::make();
    impl->start();
}

AudioPlayer::~AudioPlayer()
{
    {
        const std::lock_guard hold(impl->lock);
        impl->quitting = true;
    }
    impl->wake.notify_one();
    impl->reader.join();
    if (impl->queue != nullptr)
        impl->queue->stop();
}

void AudioPlayer::prepare(double sampleRate, int channels)
{
    {
        const std::lock_guard filling(impl->fillLock);
        impl->rate = sampleRate;
        impl->channels = std::max(1, channels);
        const auto fadeFrames = std::chrono::duration<double>(impl->options.fade).count() * sampleRate;
        impl->fadeStep = fadeFrames >= 1.0 ? 1.0 / fadeFrames : 1.0;
        const auto capacity = std::max<std::size_t>(
            4 * chunkFrames,
            static_cast<std::size_t>(std::chrono::duration<double>(impl->options.readAhead).count() * sampleRate));
        impl->rings.clear();
        for (int c = 0; c < impl->channels; ++c)
            impl->rings.push_back(std::make_unique<LockFreeFifo<float>>(capacity));
        impl->chunks = std::make_unique<LockFreeFifo<Chunk>>(capacity / chunkFrames * 2 + 16);
        impl->playable = 0;
        impl->fill = {};
        impl->current = 0;
        impl->holding = false;
        impl->gain = 0.0;
    }
    // What was playing carries on from where it was, at the new rate.
    impl->ask({.from = impl->heardPosition.load()}, true);
}

void AudioPlayer::play(std::shared_ptr<AudioPlayerSource> source, std::int64_t from, std::int64_t to,
                       std::int64_t loopFrom)
{
    impl->ask({.source = std::move(source), .from = from, .to = to, .loopFrom = loopFrom}, false);
}

void AudioPlayer::seek(std::int64_t to)
{
    impl->ask({.from = to}, true);
}

void AudioPlayer::stop()
{
    impl->ask({}, false);
}

bool AudioPlayer::playing() const noexcept
{
    const auto want = impl->wanted.load();
    return impl->sounding.load() || (want != 0 && want != impl->finishedGeneration.load());
}

std::int64_t AudioPlayer::position() const noexcept
{
    return impl->heardPosition.load();
}

std::size_t AudioPlayer::ready() const noexcept
{
    return static_cast<std::size_t>(std::max<std::int64_t>(0, impl->playable.load()));
}

void AudioPlayer::render(float* const* outputs, int channels, int frames) noexcept
{
    impl->render(outputs, channels, frames);
}

}
