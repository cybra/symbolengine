#include "symbolengine/devices/AudioBridge.h"

#include <symbolengine/core/Resampler.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

// The buffer between the two clocks is held at its target by a proportional and integral loop on how full it is,
// smoothed over half a second, whatever the blocks' size, which the loop's damping needs to stay well under the 4.7
// seconds it would ring at; the integral part settles at the two clocks' drift, which is what is reported. The target
// is at least what an input block and an output block need held at once. How
// full it is is read at the output block's own time: the input written by then, counting what was captured since the
// input's last block at its rate, less what has been taken, so the drift shows as it builds and not only when a whole
// block more has come, about once every two minutes at a hundred parts per million. Both streams' blocks carry times
// from the same steady clock; without them the frames in the buffer are read as they are. The
// loop's natural frequency is 0.3 radians a second, damped at 0.7, so a drift of 1% settles in under a minute with the
// buffer never more than a few milliseconds off, and the ratio wavers by a few hundred parts per million at most.
namespace symbolengine
{

namespace
{
// Enough for half a second at 384 kHz, so the buffer is made once, before either stream's rate is known.
constexpr double mostRate = 384000.0;
constexpr double naturalFrequency = 0.3, damping = 0.7;
constexpr double integralGain = naturalFrequency * naturalFrequency * 1e6;    // ppm a second, for each second off
constexpr double proportionalGain = 2.0 * damping * naturalFrequency * 1e6;   // ppm, for each second off
constexpr double mostCorrection = 50000.0;                                    // ppm
constexpr double smoothingSeconds = 0.5;
constexpr std::size_t carryFrames = 8192;

// What the buffer holds past an input block and an output block, for the two streams' blocks not arriving evenly.
constexpr double marginFrames = 64.0;
}

struct AudioBridge::Impl
{
    explicit Impl(Options o) : options(o), channels(std::max(1, o.channels)), inputSide(*this), outputSide(*this)
    {
        room = static_cast<std::size_t>(std::chrono::duration<double>(options.capacity).count() * mostRate);
        ring.assign(static_cast<std::size_t>(channels), std::vector<float>(room));
        carry.assign(static_cast<std::size_t>(channels), std::vector<float>(carryFrames));
        for (auto& channel : carry)
            carryPointers.push_back(channel.data());
        madePointers.resize(static_cast<std::size_t>(channels));
        offsetPointers.resize(static_cast<std::size_t>(channels));
    }

    struct Input final : AudioCallback
    {
        explicit Input(Impl& i) : impl(i) {}
        void prepare(const AudioStreamInfo& info) override
        {
            impl.inputLatency = info.inputLatency > 0 && info.sampleRate > 0.0 ? 1e9 * static_cast<double>(info.inputLatency) / info.sampleRate : 0.0;
            impl.inputBlock = info.maxBlock;
            impl.inputRate = info.sampleRate;
            impl.makeResampler();
        }
        void process(const AudioBlock& block) noexcept override { impl.take(block); }
        Impl& impl;
    };

    struct Output final : AudioCallback
    {
        explicit Output(Impl& i) : impl(i) {}
        void prepare(const AudioStreamInfo& info) override { impl.prepareOutput(info); }
        void process(const AudioBlock& block) noexcept override { impl.give(block); }
        Impl& impl;
    };

    std::size_t readable() const noexcept
    {
        return static_cast<std::size_t>(ringIn.load(std::memory_order_acquire) -
                                        ringOut.load(std::memory_order_acquire));
    }

    // What the output's target has to be at the least, in input frames: an input block and an output block held at once.
    double framesNeeded(double in, double out) const noexcept
    {
        return inputBlock.load() + (out > 0.0 ? outputBlock.load() * in / out : 0.0) + marginFrames;
    }

    // The input's thread: a block in whole or not at all. Its frames are there for the output only once every channel
    // has them, so the channels can't part, whichever thread is held up where.
    void take(const AudioBlock& block) noexcept
    {
        // The buffer holds its capacity at the input's own rate, though it was made for the highest, and at least what
        // the target needs.
        const auto frames = static_cast<std::size_t>(block.frames);
        const auto in = inputRate.load();
        const auto limit = static_cast<std::size_t>(std::max(
            std::chrono::duration<double>(options.capacity).count() * in, 2.0 * framesNeeded(in, outputRate.load())));
        const auto held = readable();
        if (room - held < frames || held + frames > limit)
        {
            overflowed = true;
            return;
        }

        const auto writeAt = static_cast<std::size_t>(ringIn.load(std::memory_order_relaxed) % room);
        const auto first = std::min(frames, room - writeAt);
        for (int c = 0; c < channels; ++c)
        {
            const auto* from = block.inputChannels == 0 ? nullptr
                                                        : block.inputs[std::min(c, block.inputChannels - 1)];
            auto& channel = ring[static_cast<std::size_t>(c)];
            if (from != nullptr)
            {
                std::copy_n(from, first, channel.begin() + static_cast<std::ptrdiff_t>(writeAt));
                std::copy_n(from + first, frames - first, channel.begin());
            }
            else
            {
                std::fill_n(channel.begin() + static_cast<std::ptrdiff_t>(writeAt), first, 0.0f);
                std::fill_n(channel.begin(), frames - first, 0.0f);
            }
        }
        ringIn.fetch_add(frames, std::memory_order_release);

        // How much has been written and when, the block's capture time moved on by the input's latency to when its
        // last frame came, behind a sequence count so the output reads the two as one.
        const auto written = writtenTotal + static_cast<std::int64_t>(frames);
        writtenTotal = written;
        sequence.fetch_add(1, std::memory_order_acq_rel);
        writtenNow.store(written, std::memory_order_relaxed);
        writtenAt.store(block.inputTime >= 0 ? block.inputTime + static_cast<std::int64_t>(inputLatency.load()) : -1,
                        std::memory_order_relaxed);
        sequence.fetch_add(1, std::memory_order_release);
    }

    // The input written by a time on the steady clock, counting what was captured since its last block; or -1 when
    // the blocks carry no times, or the input's thread is partway through writing them and held up there, which no
    // audio thread waits out.
    double writtenBy(std::int64_t time, double rate) const noexcept
    {
        constexpr int tries = 8;
        for (int i = 0; i < tries; ++i)
        {
            const auto before = sequence.load(std::memory_order_acquire);
            if ((before & 1) != 0)
                continue;
            const auto written = writtenNow.load(std::memory_order_relaxed);
            const auto at = writtenAt.load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (sequence.load(std::memory_order_relaxed) != before)
                continue;
            if (at < 0 || time < 0)
                return -1.0;
            return static_cast<double>(written) + static_cast<double>(time - at) * 1e-9 * rate;
        }
        return -1.0;
    }

    // From either stream's control thread: a resampler for the rates they have now, cut off for them, which the output
    // takes at its next block. Each is kept until the bridge goes, so the audio thread frees nothing, and used again for
    // the same rates.
    void makeResampler()
    {
        const auto in = inputRate.load(), out = outputRate.load();
        if (in <= 0.0 || out <= 0.0)
            return;

        const std::lock_guard hold(resamplersLock);
        const auto ratio = out / in;
        const auto same = std::find_if(resamplers.begin(), resamplers.end(),
                                       [ratio](const auto& kept) { return kept.first == ratio; });
        if (same == resamplers.end())
        {
            resamplers.emplace_back(ratio, std::make_unique<Resampler>(channels, ratio, carryFrames));
            freshResampler = resamplers.back().second.get();
        }
        else
        {
            freshResampler = same->second.get();
        }
    }

    // The output's control thread, before its first block.
    void prepareOutput(const AudioStreamInfo& info)
    {
        outputLatency = info.outputLatency > 0 && info.sampleRate > 0.0 ? 1e9 * static_cast<double>(info.outputLatency) / info.sampleRate : 0.0;
        outputBlock = info.maxBlock;
        outputRate = info.sampleRate;
        made.assign(static_cast<std::size_t>(channels), std::vector<float>(static_cast<std::size_t>(std::max(1, info.maxBlock))));
        for (std::size_t c = 0; c < made.size(); ++c)
            madePointers[c] = made[c].data();
        makeResampler();
    }

    void startAgain() noexcept
    {
        // Let go of once, being the same frames on every channel.
        const auto held = readable();
        ringOut.fetch_add(held, std::memory_order_release);
        consumedTotal += static_cast<std::int64_t>(held);
        consumedTotal += static_cast<std::int64_t>(carried);
        carried = 0;
        if (resampler != nullptr)
            resampler->reset();
        priming = true;
        filtered = 0.0;
        settledNow = false;
    }

    // The output's thread.
    void give(const AudioBlock& block) noexcept
    {
        const auto in = inputRate.load(), out = outputRate.load();
        const auto frames = static_cast<std::size_t>(block.frames);

        if (auto* const fresh = freshResampler.exchange(nullptr))
        {
            resampler = fresh;
            startAgain();
        }

        if (in <= 0.0 || out <= 0.0 || resampler == nullptr || frames > made.front().size())
            return;

        if (overflowed.exchange(false))
        {
            ++overruns;
            startAgain();
        }

        // Starting, the buffer fills to the delay before anything is taken from it. How full it is: at the block's own
        // time, its time to be heard less the output's latency, when the blocks carry times.
        const auto target = std::max(std::chrono::duration<double>(options.delay).count() * in, framesNeeded(in, out));
        const auto held = static_cast<double>(readable() + carried);
        const auto now = block.outputTime >= 0 ? block.outputTime - static_cast<std::int64_t>(outputLatency) : -1;

        // Filling waits for frames that are there, not those the clock says are on their way. Output blocks long against
        // the delay find more than it there, which is let go of before anything plays, so the delay starts where it is
        // held rather than the loop having to bring it there, and past it.
        if (priming)
        {
            if (held < target)
                return;
            const auto over = std::min(static_cast<std::size_t>(held - target), readable());
            ringOut.fetch_add(over, std::memory_order_release);
            consumedTotal += static_cast<std::int64_t>(over);
        }

        const auto written = writtenBy(now, in);
        const auto fill =
            written >= 0.0 ? written - static_cast<double>(consumedTotal) : static_cast<double>(readable() + carried);
        if (priming)
        {
            priming = false;
            filtered = fill;
        }

        const auto seconds = static_cast<double>(frames) / out;
        filtered += (fill - filtered) * (1.0 - std::exp(-seconds / smoothingSeconds));

        const auto off = (filtered - target) / in;
        integral = std::clamp(integral + integralGain * off * seconds, -mostCorrection, mostCorrection);
        const auto correction = std::clamp(integral + proportionalGain * off, -mostCorrection, mostCorrection);
        resampler->setRatio(out / in / (1.0 + correction * 1e-6));

        std::size_t produced = 0;
        while (produced < frames)
        {
            const auto take = std::min(carryFrames - carried, readable());
            const auto readAt = static_cast<std::size_t>(ringOut.load(std::memory_order_relaxed) % room);
            const auto first = std::min(take, room - readAt);
            for (std::size_t c = 0; c < ring.size(); ++c)
            {
                std::copy_n(ring[c].begin() + static_cast<std::ptrdiff_t>(readAt), first,
                            carry[c].begin() + static_cast<std::ptrdiff_t>(carried));
                std::copy_n(ring[c].begin(), take - first,
                            carry[c].begin() + static_cast<std::ptrdiff_t>(carried + first));
            }
            ringOut.fetch_add(take, std::memory_order_release);
            carried += take;
            for (std::size_t c = 0; c < offsetPointers.size(); ++c)
                offsetPointers[c] = madePointers[c] + produced;
            const auto done = resampler->process(carryPointers.data(), carried, offsetPointers.data(), frames - produced);
            consumedTotal += static_cast<std::int64_t>(done.consumed);
            if (done.consumed > 0)
            {
                for (auto& channel : carry)
                    std::memmove(channel.data(), channel.data() + done.consumed, (carried - done.consumed) * sizeof(float));
                carried -= done.consumed;
            }
            produced += done.produced;
            if (done.consumed == 0 && done.produced == 0)
                break;
        }

        for (int c = 0; c < block.outputChannels; ++c)
        {
            const auto& from = made[static_cast<std::size_t>(std::min(c, channels - 1))];
            for (std::size_t i = 0; i < produced; ++i)
                block.outputs[c][i] += from[i] * options.gain;
        }

        if (produced < frames)
        {
            ++underruns;
            startAgain();
            return;
        }
        delayNow = filtered / in;
        settledNow = std::abs(off) < 0.001;
        driftNow = integral;
    }

    Options options;
    int channels;
    Input inputSide;
    Output outputSide;
    std::atomic<double> inputRate{0.0}, outputRate{0.0}, inputLatency{0.0};
    std::atomic<int> inputBlock{0}, outputBlock{0};
    double outputLatency = 0.0;

    // The frames between the two threads, each channel's kept at the same place, and how many have gone in and out.
    std::vector<std::vector<float>> ring;
    std::size_t room = 0;
    std::atomic<std::uint64_t> ringIn{0}, ringOut{0};

    // What the input has written and when, for the output to read as one; the input's own count, and the output's of
    // what it has taken.
    std::atomic<std::uint64_t> sequence{0};
    std::atomic<std::int64_t> writtenNow{0}, writtenAt{-1};
    std::int64_t writtenTotal = 0, consumedTotal = 0;
    std::atomic<bool> overflowed{false};
    std::atomic<std::int64_t> underruns{0}, overruns{0};
    std::atomic<double> driftNow{0.0}, delayNow{0.0};
    std::atomic<bool> settledNow{false};

    // The resamplers made for each ratio, the one the output takes next, and the output's own.
    std::mutex resamplersLock;
    std::vector<std::pair<double, std::unique_ptr<Resampler>>> resamplers;
    std::atomic<Resampler*> freshResampler{nullptr};
    Resampler* resampler = nullptr;
    std::vector<std::vector<float>> carry, made;
    std::vector<const float*> carryPointers;
    std::vector<float*> madePointers, offsetPointers;
    std::size_t carried = 0;
    bool priming = true;
    double filtered = 0.0, integral = 0.0;
};

AudioBridge::AudioBridge(Options options) : impl(std::make_unique<Impl>(options)) {}
AudioBridge::~AudioBridge() = default;

AudioCallback& AudioBridge::input() noexcept
{
    return impl->inputSide;
}

AudioCallback& AudioBridge::output() noexcept
{
    return impl->outputSide;
}

AudioBridge::State AudioBridge::state() const noexcept
{
    return {impl->driftNow.load(), impl->delayNow.load(), impl->settledNow.load(), impl->underruns.load(),
            impl->overruns.load()};
}

}
