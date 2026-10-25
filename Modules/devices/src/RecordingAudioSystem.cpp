#include "symbolengine/devices/RecordingAudioSystem.h"

#include "AudioRecordFormat.h"

#include <symbolengine/core/Json.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

// The recorder keeps the audio thread to atomics: what a stream's device does is counted there, and written down by
// the recorder's own thread, which also reads the devices again once a notice has settled.
namespace symbolengine
{

namespace
{
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

struct Watched
{
    std::string id;
    std::atomic<std::int64_t> lastCycle{-1}, blockNanoseconds{0};
    std::atomic<std::int64_t> expectInput{-1}, expectOutput{-1}, gapFrames{0};
    std::atomic<int> gaps{0}, flagged{0}, changes{0}, reopens{0};
    std::atomic<int> lossWhy{-1};
    std::atomic<std::int64_t> lossCode{0};
    std::atomic<bool> started{false};
    bool stalled = false;
};
}

struct RecordingAudioSystem::Impl final : AudioSystemListener
{
    std::shared_ptr<AudioSystem> real;
    std::function<void(const std::string&)> write;
    Clock::time_point began = Clock::now();

    std::mutex writing;
    std::mutex lock;
    std::condition_variable wake;
    bool quitting = false, noticed = true;
    Clock::time_point noticedAt = Clock::now();
    std::string lastDevices;
    std::vector<std::shared_ptr<Watched>> watched;

    std::mutex listening;
    std::vector<AudioSystemListener*> listeners;
    std::thread thread;

    void line(Object fields)
    {
        Object out{{"seconds", std::chrono::duration<double>(Clock::now() - began).count()}, {"system", real->name()}};
        for (auto& member : fields)
            out.set(member.name(), member.value);
        const auto text = json::write(Value(std::move(out)));
        const std::lock_guard hold(writing);
        if (write)
            write(text);
    }

    void systemChanged(AudioSystemChange change) noexcept override
    {
        line({{"event", "notice"}, {"change", devices::detail::nameOf(change)}});
        {
            const std::lock_guard hold(lock);
            noticed = true;
            noticedAt = Clock::now();
        }
        wake.notify_all();
        const std::lock_guard hold(listening);
        for (auto* listener : listeners)
            listener->systemChanged(change);
    }

    // The devices, their descriptions and the defaults, written when they differ from what was written last.
    void snapshot()
    {
        Array devices;
        for (const auto& id : real->deviceIds())
            devices.push_back(devices::detail::toValue(real->describe(id)));
        Object now{{"event", "devices"},
                   {"devices", std::move(devices)},
                   {"defaultInput", real->defaultDevice(AudioDirection::input)},
                   {"defaultOutput", real->defaultDevice(AudioDirection::output)},
                   {"inputMuted", real->inputMuted()}};
        auto text = json::write(Value(now));
        if (text == lastDevices)
            return;
        lastDevices = std::move(text);
        line(std::move(now));
    }

    void report(Watched& stream)
    {
        if (const auto why = stream.lossWhy.exchange(-1); why >= 0)
            line({{"event", "lost"},
                  {"device", stream.id},
                  {"why", devices::detail::nameOf(static_cast<AudioLoss>(why))},
                  {"code", stream.lossCode.load()}});
        for (auto n = stream.changes.exchange(0); n > 0; --n)
            line({{"event", "changed"}, {"device", stream.id}});
        for (auto n = stream.reopens.exchange(0); n > 0; --n)
            line({{"event", "reopen"}, {"device", stream.id}});
        if (const auto n = stream.gaps.exchange(0); n > 0)
            line({{"event", "gap"}, {"device", stream.id}, {"count", n}, {"frames", stream.gapFrames.exchange(0)}});
        if (const auto n = stream.flagged.exchange(0); n > 0)
            line({{"event", "flagged gap"}, {"device", stream.id}, {"count", n}});

        // No blocks for three blocks' time, or a tenth of a second, while started: a stall, and its end.
        const auto last = stream.lastCycle.load();
        if (stream.started.load() && last >= 0)
        {
            const auto quiet = real->now() - last;
            const auto limit = std::max<std::int64_t>(3 * stream.blockNanoseconds.load(), 100'000'000);
            if (!stream.stalled && quiet > limit)
            {
                stream.stalled = true;
                line({{"event", "stall"}, {"device", stream.id}});
            }
            else if (stream.stalled && quiet <= limit)
            {
                stream.stalled = false;
                line({{"event", "resumed"}, {"device", stream.id}});
            }
        }
    }

    void loop()
    {
        std::unique_lock hold(lock);
        while (!quitting)
        {
            wake.wait_for(hold, 50ms);
            if (quitting)
                break;
            const bool settle = noticed && Clock::now() - noticedAt >= 100ms;
            if (settle)
                noticed = false;
            auto streams = watched;
            hold.unlock();
            if (settle)
                snapshot();
            for (auto& stream : streams)
                report(*stream);
            streams.clear();
            hold.lock();
            // A stream let go of, its last events written, is watched no more.
            std::erase_if(watched, [](const std::shared_ptr<Watched>& w) { return w.use_count() == 1 && !w->started; });
        }
    }
};

namespace
{
class RecordingClient final : public AudioSystemClient
{
public:
    RecordingClient(AudioSystemClient& c, std::shared_ptr<Watched> w, std::shared_ptr<AudioSystem> s)
        : client(c), watched(std::move(w)), system(std::move(s))
    {
    }

    void cycle(const AudioSystemCycle& given) noexcept override
    {
        auto& w = *watched;
        w.lastCycle = system->now();
        const auto frames = static_cast<std::int64_t>(given.frames);
        const auto position = given.inputChannels > 0 ? given.inputPosition : given.outputPosition;
        auto& expect = given.inputChannels > 0 ? w.expectInput : w.expectOutput;
        if (position >= 0)
        {
            const auto expected = expect.exchange(position + frames);
            if (expected >= 0 && position > expected)
            {
                w.gapFrames += position - expected;
                ++w.gaps;
            }
        }
        else if (given.inputGap || given.outputGap)
            ++w.flagged;
        client.cycle(given);
    }

    void changed() noexcept override
    {
        ++watched->changes;
        client.changed();
    }

    void reopen() noexcept override
    {
        ++watched->reopens;
        client.reopen();
    }

    void lost(AudioLoss why, std::int64_t code) noexcept override
    {
        watched->lossCode = code;
        watched->lossWhy = static_cast<int>(why);
        client.lost(why, code);
    }

private:
    AudioSystemClient& client;
    std::shared_ptr<Watched> watched;
    std::shared_ptr<AudioSystem> system;
};

class RecordingStream final : public AudioSystemStream
{
public:
    RecordingStream(std::unique_ptr<AudioSystemStream> r, std::shared_ptr<Watched> w,
                    std::shared_ptr<RecordingAudioSystem::Impl> i)
        : real(std::move(r)), watched(std::move(w)), impl(std::move(i))
    {
    }

    ~RecordingStream() override { watched->started = false; }

    AudioStreamInfo info() const override { return real->info(); }

    bool start(AudioSystemClient& to, AudioError& error) override
    {
        client = std::make_unique<RecordingClient>(to, watched, impl->real);
        const auto now = real->info();
        watched->blockNanoseconds =
            now.sampleRate > 0.0 ? static_cast<std::int64_t>(1e9 * now.maxBlock / now.sampleRate) : 0;
        watched->expectInput = -1;
        watched->expectOutput = -1;
        watched->lastCycle = impl->real->now();
        watched->started = true;
        const bool started = real->start(*client, error);
        if (!started)
            watched->started = false;
        impl->line(started ? Object{{"event", "start"}, {"device", watched->id}, {"rate", now.sampleRate},
                                    {"block", now.maxBlock}}
                           : Object{{"event", "start failed"}, {"device", watched->id}, {"message", error.message}});
        return started;
    }

    void stop() noexcept override
    {
        watched->started = false;
        real->stop();
        impl->line({{"event", "stop"}, {"device", watched->id}});
    }

    AudioError showSettings() override { return real->showSettings(); }
    AudioWorkgroup workgroup() const override { return real->workgroup(); }

private:
    std::unique_ptr<AudioSystemStream> real;
    std::shared_ptr<Watched> watched;
    std::shared_ptr<RecordingAudioSystem::Impl> impl;
    std::unique_ptr<RecordingClient> client;
};
}

RecordingAudioSystem::RecordingAudioSystem(std::shared_ptr<AudioSystem> real,
                                           std::function<void(const std::string& line)> write)
    : impl(std::make_shared<Impl>())
{
    impl->real = std::move(real);
    impl->write = std::move(write);
    impl->line({{"event", "recording"}, {"system", impl->real->name()}});
    impl->real->addListener(*impl);
    impl->snapshot();
    {
        const std::lock_guard hold(impl->lock);
        impl->noticed = false;
    }
    impl->thread = std::thread([i = impl.get()] { i->loop(); });
}

RecordingAudioSystem::~RecordingAudioSystem()
{
    impl->real->removeListener(*impl);
    {
        const std::lock_guard hold(impl->lock);
        impl->quitting = true;
    }
    impl->wake.notify_all();
    impl->thread.join();
    auto streams = impl->watched;
    for (auto& stream : streams)
        impl->report(*stream);
}

std::string RecordingAudioSystem::name() const
{
    return impl->real->name();
}

std::int64_t RecordingAudioSystem::now() const noexcept
{
    return impl->real->now();
}

std::vector<std::string> RecordingAudioSystem::deviceIds()
{
    return impl->real->deviceIds();
}

AudioDeviceInfo RecordingAudioSystem::describe(const std::string& id)
{
    return impl->real->describe(id);
}

std::string RecordingAudioSystem::defaultDevice(AudioDirection direction)
{
    return impl->real->defaultDevice(direction);
}

bool RecordingAudioSystem::inputMuted()
{
    return impl->real->inputMuted();
}

std::unique_ptr<AudioSystemStream> RecordingAudioSystem::open(const std::string& id, const AudioStreamRequest& request,
                                                              AudioError& error)
{
    Array inputs, outputs;
    for (const auto channel : request.inputChannels)
        inputs.emplace_back(channel);
    for (const auto channel : request.outputChannels)
        outputs.emplace_back(channel);
    auto opened = impl->real->open(id, request, error);
    impl->line({{"event", opened != nullptr ? "open" : "open failed"},
                {"device", id},
                {"inputs", std::move(inputs)},
                {"outputs", std::move(outputs)},
                {"rate", request.sampleRate},
                {"block", request.blockSize},
                {"exclusive", request.exclusive},
                {"message", error.message}});
    if (opened == nullptr)
        return nullptr;
    auto watched = std::make_shared<Watched>();
    watched->id = id;
    {
        const std::lock_guard hold(impl->lock);
        impl->watched.push_back(watched);
    }
    return std::make_unique<RecordingStream>(std::move(opened), std::move(watched), impl);
}

void RecordingAudioSystem::addListener(AudioSystemListener& listener)
{
    const std::lock_guard hold(impl->listening);
    impl->listeners.push_back(&listener);
}

void RecordingAudioSystem::removeListener(AudioSystemListener& listener)
{
    const std::lock_guard hold(impl->listening);
    std::erase(impl->listeners, &listener);
}

}
