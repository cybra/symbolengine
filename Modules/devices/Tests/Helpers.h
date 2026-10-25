#pragma once

#include <symbolengine/devices/AudioStream.h>
#include <symbolengine/devices/FakeAudioSystem.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

// Waits up to five seconds for what another thread does, saying whether it came.
template <typename Condition>
bool soon(Condition condition, std::chrono::milliseconds limit = 5000ms)
{
    const auto until = std::chrono::steady_clock::now() + limit;
    while (!condition())
    {
        if (std::chrono::steady_clock::now() > until)
            return false;
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

// The events a stream delivers, kept for a test to look through, with the thread each came on.
class Events
{
public:
    std::function<void(const symbolengine::AudioEvent&)> handler()
    {
        return [this](const symbolengine::AudioEvent& event) {
            if (inHandler.exchange(true))
                overlapped = true;
            {
                const std::lock_guard hold(lock);
                kept.push_back(event);
                threads.push_back(std::this_thread::get_id());
            }
            inHandler = false;
        };
    }

    std::vector<symbolengine::AudioEvent> all() const
    {
        const std::lock_guard hold(lock);
        return {kept.begin(), kept.end()};
    }

    std::vector<symbolengine::AudioEvent> of(symbolengine::AudioEvent::Kind kind) const
    {
        std::vector<symbolengine::AudioEvent> found;
        for (const auto& event : all())
            if (event.kind == kind)
                found.push_back(event);
        return found;
    }

    std::size_t count(symbolengine::AudioEvent::Kind kind) const { return of(kind).size(); }

    bool waitFor(symbolengine::AudioEvent::Kind kind, std::size_t times = 1,
                 std::chrono::milliseconds limit = 5000ms) const
    {
        return soon([&] { return count(kind) >= times; }, limit);
    }

    std::vector<std::thread::id> deliveredOn() const
    {
        const std::lock_guard hold(lock);
        return threads;
    }

    std::atomic<bool> overlapped{false};

private:
    mutable std::mutex lock;
    std::deque<symbolengine::AudioEvent> kept;
    std::vector<std::thread::id> threads;
    std::atomic<bool> inHandler{false};
};

// A callback that keeps what it was given: the order of its calls, every block's frames and positions, and its
// inputs' first samples, without allocating on the audio thread.
class Recording final : public symbolengine::AudioCallback
{
public:
    enum Call : int
    {
        prepared = 1,
        processed = 2,
        stoppedCall = 3
    };

    explicit Recording(std::size_t room = 1 << 20) : calls(room), blocks(room) {}

    void prepare(const symbolengine::AudioStreamInfo& info) override
    {
        note(prepared);
        rate = info.sampleRate;
        maxBlock = info.maxBlock;
        ++prepares;
    }

    void process(const symbolengine::AudioBlock& block) noexcept override
    {
        note(processed);
        if (onProcess)
            onProcess(block);
        const auto at = blockCount.fetch_add(1);
        if (at < blocks.size())
            blocks[at] = {block.frames, block.inputPosition, block.outputPosition, block.sampleRate,
                          block.inputChannels > 0 ? block.inputs[0][0] : 0.0f};
        frames += block.frames;
        largest = std::max(largest.load(), block.frames);
        if (block.frames == 0)
            emptyBlocks++;
    }

    void stopped() override
    {
        note(stoppedCall);
        ++stops;
    }

    struct Block
    {
        int frames = 0;
        std::int64_t inputPosition = 0, outputPosition = 0;
        double sampleRate = 0.0;
        float firstInput = 0.0f;
    };

    std::vector<Block> kept() const
    {
        return {blocks.begin(), blocks.begin() + static_cast<std::ptrdiff_t>(std::min(blockCount.load(), blocks.size()))};
    }

    // The calls in order, each one of Call.
    std::vector<int> order() const
    {
        return {calls.begin(), calls.begin() + static_cast<std::ptrdiff_t>(std::min(callCount.load(), calls.size()))};
    }

    std::function<void(const symbolengine::AudioBlock&)> onProcess;
    std::atomic<std::int64_t> frames{0};
    std::atomic<int> largest{0}, emptyBlocks{0}, prepares{0}, stops{0};
    std::atomic<double> rate{0.0};
    std::atomic<int> maxBlock{0};

private:
    void note(Call call) noexcept
    {
        const auto at = callCount.fetch_add(1);
        if (at < calls.size())
            calls[at] = call;
    }

    std::vector<int> calls;
    std::atomic<std::size_t> callCount{0};
    std::vector<Block> blocks;
    std::atomic<std::size_t> blockCount{0};
};

// A fake system with one device, its inputs and outputs named, ready for a stream.
inline symbolengine::AudioDeviceInfo fakeDevice(std::string id, int inputs = 2, int outputs = 2,
                                                std::string name = {})
{
    symbolengine::AudioDeviceInfo info;
    info.id = id;
    info.name = name.empty() ? id : name;
    for (int c = 0; c < inputs; ++c)
        info.inputNames.push_back("Input " + std::to_string(c + 1));
    for (int c = 0; c < outputs; ++c)
        info.outputNames.push_back("Output " + std::to_string(c + 1));
    return info;
}

inline symbolengine::AudioStreamRequest stereo(bool input = true, bool output = true)
{
    symbolengine::AudioStreamRequest request;
    if (input)
        request.inputChannels = {0, 1};
    if (output)
        request.outputChannels = {0, 1};
    return request;
}

// Options for a stream under test: quick to give up on a device, events kept.
inline symbolengine::AudioStream::Options quick(Events& events)
{
    symbolengine::AudioStream::Options options;
    options.onEvent = events.handler();
    options.callTimeout = 1000ms;
    options.firstBlockTimeout = 2000ms;
    options.settle = 20ms;
    options.wakeDelay = 3000ms;
    options.watchEvery = 5ms;
    return options;
}

// The fake's counting input, as FakeAudioDevice gives it without one of a test's own.
inline float counting(int channel, std::int64_t frame)
{
    const auto value = static_cast<float>((frame + 1) % (std::int64_t{1} << 24)) / 16777216.0f;
    return channel % 2 == 0 ? value : -value;
}
