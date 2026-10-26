#include "symbolengine/devices/FakeAudioSystem.h"

#include "AudioRecordFormat.h"

#include <symbolengine/core/Json.h>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <random>
#include <thread>

// The fake keeps each device's state apart from the device object a test holds, and each open stream's state apart
// from the stream object the devices module holds, so that a cycle given from a test's thread, a device's own thread
// or a late thread after a stop never reaches freed memory, whatever order things are destroyed in. A device object
// lives as long as its system, which the devices module's streams keep alive. Cycles never take a lock the devices
// module's calls into the fake take, as a real system's audio thread doesn't, except a cycle given inside start.
namespace symbolengine
{

namespace
{
using namespace std::chrono_literals;

float countingSample(int channel, std::int64_t frame)
{
    const auto value = static_cast<float>((frame + 1) % (std::int64_t{1} << 24)) / 16777216.0f;
    return channel % 2 == 0 ? value : -value;
}

void fillDefaults(AudioDeviceInfo& info)
{
    if (info.sampleRates.empty())
        info.sampleRates = {44100.0, 48000.0, 88200.0, 96000.0};
    if (info.currentRate == 0.0)
        info.currentRate = 48000.0;
    if (info.blockSizes.empty())
        info.blockSizes = {64, 128, 256, 512, 1024, 2048};
    if (info.currentBlock == 0)
        info.currentBlock = 512;
}
}

struct FakeStreamState;

struct FakeAudioDevice::State
{
    FakeAudioDevice* owner = nullptr;
    FakeAudioSystem::Impl* system = nullptr;

    // The device's own lock, which every call about it takes, as a system's property calls do.
    mutable std::mutex lock;
    AudioDeviceInfo info;
    AudioStreamRequest lastRequest;
    FakeRun runSettings;
    bool runs = false;
    bool listed = true;
    bool quiet = false;

    std::atomic<int> opens{0}, starts{0}, stops{0};

    // The stream open on it, behind a lock of its own that is never held while the devices module is called.
    mutable std::mutex streamLock;
    std::shared_ptr<FakeStreamState> stream;

    mutable std::mutex outputLock;
    std::vector<std::vector<float>> lastOutput;

    std::shared_ptr<FakeStreamState> current() const
    {
        const std::lock_guard hold(streamLock);
        return stream;
    }
};

struct FakeAudioSystem::Impl
{
    std::string name;
    std::atomic<std::int64_t> clock{0};
    std::atomic<int> describes{0}, opens{0}, sets{0};
    std::atomic<bool> muted{false};

    std::mutex lock;
    std::vector<std::unique_ptr<FakeAudioDevice>> devices;
    std::vector<std::shared_ptr<FakeAudioDevice::State>> states;
    std::string defaultInput, defaultOutput;

    // Listeners are called with no lock held; removing one waits for the calls under way to end.
    std::mutex listenerLock;
    std::condition_variable listenersIdle;
    std::vector<AudioSystemListener*> listeners;
    int callsUnderWay = 0;

    void tell(AudioSystemChange change)
    {
        std::vector<AudioSystemListener*> now;
        {
            const std::lock_guard hold(listenerLock);
            now = listeners;
            ++callsUnderWay;
        }
        for (auto* listener : now)
            listener->systemChanged(change);
        {
            const std::lock_guard hold(listenerLock);
            --callsUnderWay;
        }
        listenersIdle.notify_all();
    }

    std::shared_ptr<FakeAudioDevice::State> find(const std::string& id, bool listedOnly = true)
    {
        const std::lock_guard hold(lock);
        for (auto& state : states)
        {
            const std::lock_guard device(state->lock);
            if (state->info.id == id && (state->listed || !listedOnly))
                return state;
        }
        return nullptr;
    }
};

struct FakeStreamState
{
    std::shared_ptr<FakeAudioDevice::State> device;
    FakeAudioSystem::Impl* system = nullptr;
    AudioStreamRequest request;
    AudioStreamInfo opened;

    std::atomic<AudioSystemClient*> client{nullptr};

    // One cycle at a time, whoever gives it.
    std::mutex cycleLock;
    std::int64_t position = 0;
    std::size_t capacity = 0;
    std::vector<std::vector<float>> inputs, outputs;
    std::vector<const float*> inputPointers;
    std::vector<float*> outputPointers;
    std::minstd_rand noise{7};

    std::thread runner;
    std::atomic<bool> running{false};
    std::thread late;

    // Gives a cycle to the client, or to the client given; deviceLocked when the caller holds the device's lock.
    bool deliver(const FakeCycle& cycle, AudioSystemClient* to = nullptr, bool deviceLocked = false)
    {
        const std::lock_guard hold(cycleLock);
        auto* target = to != nullptr ? to : client.load();
        if (target == nullptr)
            return false;

        double rate = 0.0, ppm = 0.0;
        if (deviceLocked)
        {
            rate = device->info.currentRate;
            ppm = device->runSettings.ppm;
        }
        else
        {
            const std::lock_guard locked(device->lock);
            rate = device->info.currentRate;
            ppm = device->runSettings.ppm;
        }

        const auto frames = cycle.frames != 0 ? cycle.frames : static_cast<std::size_t>(opened.maxBlock);
        if (frames > capacity)
            return false;

        position += cycle.jump;
        const auto& input = device->owner->input;
        for (std::size_t c = 0; c < inputs.size(); ++c)
        {
            const auto channel = request.inputChannels[c];
            for (std::size_t i = 0; i < frames; ++i)
            {
                const auto at = position + static_cast<std::int64_t>(i);
                inputs[c][i] = cycle.inputSilent ? static_cast<float>(noise()) / 1e9f
                                                 : (input ? input(channel, at) : countingSample(channel, at));
            }
        }
        for (auto& channel : outputs)
            std::fill(channel.begin(), channel.begin() + static_cast<std::ptrdiff_t>(frames),
                      std::numeric_limits<float>::quiet_NaN());

        AudioSystemCycle given;
        given.inputs = inputPointers.data();
        given.outputs = outputPointers.data();
        given.inputChannels = static_cast<int>(inputs.size());
        given.outputChannels = static_cast<int>(outputs.size());
        given.frames = frames;
        given.inputPosition = cycle.positions ? position : -1;
        given.outputPosition = cycle.positions ? position : -1;
        given.inputTime = system->clock.load();
        given.outputTime = given.inputTime;
        given.inputGap = cycle.inputGap;
        given.outputGap = cycle.outputGap;
        given.inputSilent = cycle.inputSilent;
        target->cycle(given);

        {
            const std::lock_guard keep(device->outputLock);
            auto& last = device->lastOutput;
            last.resize(outputs.size());
            for (std::size_t c = 0; c < outputs.size(); ++c)
                last[c].assign(outputs[c].begin(), outputs[c].begin() + static_cast<std::ptrdiff_t>(frames));
        }

        const auto advance =
            cycle.advanceNanoseconds >= 0
                ? cycle.advanceNanoseconds
                : (rate > 0.0 ? static_cast<std::int64_t>(
                                    std::llround(1e9 * static_cast<double>(frames) / (rate * (1.0 + ppm * 1e-6))))
                              : 0);
        system->clock += advance;
        position += static_cast<std::int64_t>(frames);
        return true;
    }

    // A cycle of no frames, as some systems give.
    void deliverNone()
    {
        const std::lock_guard hold(cycleLock);
        auto* to = client.load();
        if (to == nullptr)
            return;
        AudioSystemCycle given;
        given.inputs = inputPointers.data();
        given.outputs = outputPointers.data();
        given.inputChannels = static_cast<int>(inputs.size());
        given.outputChannels = static_cast<int>(outputs.size());
        given.frames = 0;
        given.inputPosition = position;
        given.outputPosition = position;
        to->cycle(given);
    }

    void run(FakeRun settings)
    {
        running = true;
        runner = std::thread([this, settings] {
            std::minstd_rand random(11);
            std::int64_t given = 0;
            auto next = std::chrono::steady_clock::now();
            const auto block = static_cast<std::size_t>(opened.maxBlock);
            while (running.load())
            {
                bool quiet = false;
                {
                    const std::lock_guard hold(device->lock);
                    quiet = device->quiet;
                }
                if (quiet || (settings.cycles != 0 && given >= settings.cycles))
                {
                    std::this_thread::sleep_for(1ms);
                    continue;
                }

                FakeCycle cycle;
                cycle.frames = settings.randomSizes ? static_cast<std::size_t>(random() % (3 * block + 1)) : block;
                cycle.advanceNanoseconds = 0;
                if (cycle.frames == 0)
                    deliverNone();
                else
                    deliver(cycle);
                ++given;

                if (settings.speed > 0.0)
                {
                    const double rate = opened.sampleRate * (1.0 + settings.ppm * 1e-6);
                    next += std::chrono::nanoseconds(static_cast<std::int64_t>(
                        1e9 * static_cast<double>(std::max<std::size_t>(cycle.frames, 1)) / rate / settings.speed));
                    std::this_thread::sleep_until(next);
                }
                else if (given % 64 == 0)
                    std::this_thread::yield();
            }
        });
    }

    void stopRunning()
    {
        running = false;
        if (runner.joinable() && runner.get_id() != std::this_thread::get_id())
            runner.join();
    }

    ~FakeStreamState()
    {
        stopRunning();
        if (late.joinable() && late.get_id() != std::this_thread::get_id())
            late.join();
        else if (late.joinable())
            late.detach();
    }
};

namespace
{
// The stream object the devices module holds.
class FakeStream final : public AudioSystemStream
{
public:
    explicit FakeStream(std::shared_ptr<FakeStreamState> s) : state(std::move(s)) {}

    ~FakeStream() override
    {
        state->stopRunning();
        state->client = nullptr;
        // A late cycle comes before the stream is gone, as a system's does before it lets go of the stream.
        if (state->late.joinable())
            state->late.join();
        const std::lock_guard hold(state->device->streamLock);
        if (state->device->stream == state)
            state->device->stream = nullptr;
    }

    AudioError showSettings() override
    {
        auto& owner = *state->device->owner;
        if (!owner.hasSettings.load())
            return AudioSystemStream::showSettings();
        ++owner.settingsShown;
        return {};
    }

    AudioStreamInfo info() const override
    {
        const std::lock_guard hold(state->device->lock);
        auto now = state->opened;
        now.sampleRate = state->device->info.currentRate;
        now.maxBlock = state->device->info.currentBlock;
        now.inputChannels = std::min(now.inputChannels, state->device->info.inputs());
        now.outputChannels = std::min(now.outputChannels, state->device->info.outputs());
        return now;
    }

    bool start(AudioSystemClient& client, AudioError& error) override
    {
        auto& device = *state->device;
        ++device.starts;
        // A slow start takes its time on the system's clock too, which runs on through it.
        for (auto left = device.owner->startMilliseconds.load(); left > 0; left -= 5)
        {
            std::this_thread::sleep_for(5ms);
            device.system->clock += std::chrono::nanoseconds(5ms).count();
        }
        if (device.owner->failStart.load())
        {
            error = {"The device didn't start.", "start", -1};
            return false;
        }

        bool runs = false;
        FakeRun settings;
        {
            const std::lock_guard hold(device.lock);
            runs = device.runs;
            settings = device.runSettings;
        }

        state->client = &client;
        if (device.owner->cycleInsideStart.load())
        {
            // The cycle comes while the device's lock is held, as some systems call back inside their start.
            const std::lock_guard hold(device.lock);
            state->deliver({}, nullptr, true);
        }
        if (device.owner->reopenInsideStart.load())
            client.reopen();
        if (runs)
            state->run(settings);
        return true;
    }

    void stop() noexcept override
    {
        auto& device = *state->device;
        ++device.stops;
        while (device.owner->hangStop.load())
            std::this_thread::sleep_for(1ms);

        auto* client = state->client.exchange(nullptr);
        state->stopRunning();
        if (client != nullptr && device.owner->callAfterStop.load())
        {
            if (state->late.joinable())
                state->late.join();
            state->late = std::thread([raw = state.get(), client] {
                std::this_thread::sleep_for(20ms);
                raw->deliver({}, client);
            });
        }
    }

private:
    std::shared_ptr<FakeStreamState> state;
};
}

// FakeAudioDevice

FakeAudioDevice::FakeAudioDevice(std::shared_ptr<State> s) : state(std::move(s))
{
    state->owner = this;
}

FakeAudioDevice::~FakeAudioDevice() = default;

void FakeAudioDevice::update(const std::function<void(AudioDeviceInfo&)>& change)
{
    const std::lock_guard hold(state->lock);
    change(state->info);
}

AudioDeviceInfo FakeAudioDevice::info() const
{
    const std::lock_guard hold(state->lock);
    return state->info;
}

bool FakeAudioDevice::cycle(const FakeCycle& cycle)
{
    const auto stream = state->current();
    return stream != nullptr && stream->deliver(cycle);
}

void FakeAudioDevice::run(FakeRun run)
{
    const std::lock_guard hold(state->lock);
    state->runs = true;
    state->runSettings = run;
}

std::vector<std::vector<float>> FakeAudioDevice::lastOutput() const
{
    const std::lock_guard hold(state->outputLock);
    return state->lastOutput;
}

void FakeAudioDevice::changed(const std::function<void(AudioDeviceInfo&)>& change)
{
    if (change)
        update(change);
    if (const auto stream = state->current())
        if (auto* client = stream->client.load())
            client->changed();
}

void FakeAudioDevice::askToReopen()
{
    if (const auto stream = state->current())
        if (auto* client = stream->client.load())
            client->reopen();
}

void FakeAudioDevice::lose(AudioLoss why, std::int64_t code)
{
    if (const auto stream = state->current())
        if (auto* client = stream->client.load())
            client->lost(why, code);
}

bool FakeAudioDevice::running() const
{
    const auto stream = state->current();
    return stream != nullptr && stream->client.load() != nullptr;
}

int FakeAudioDevice::opens() const noexcept
{
    return state->opens.load();
}

int FakeAudioDevice::starts() const noexcept
{
    return state->starts.load();
}

int FakeAudioDevice::stops() const noexcept
{
    return state->stops.load();
}

AudioStreamRequest FakeAudioDevice::lastRequest() const
{
    const std::lock_guard hold(state->lock);
    return state->lastRequest;
}

// FakeAudioSystem

FakeAudioSystem::FakeAudioSystem(std::string name) : impl(std::make_shared<Impl>())
{
    impl->name = std::move(name);
}

FakeAudioSystem::~FakeAudioSystem()
{
    // Streams still open keep their own state; their devices stop calling back.
    const std::lock_guard hold(impl->lock);
    for (auto& state : impl->states)
        if (const auto stream = state->current())
            stream->stopRunning();
}

FakeAudioDevice& FakeAudioSystem::add(AudioDeviceInfo info)
{
    fillDefaults(info);
    auto state = std::make_shared<FakeAudioDevice::State>();
    state->system = impl.get();
    state->info = std::move(info);
    auto device = std::make_unique<FakeAudioDevice>(state);
    auto& added = *device;
    {
        const std::lock_guard hold(impl->lock);
        impl->devices.push_back(std::move(device));
        impl->states.push_back(std::move(state));
    }
    impl->tell(AudioSystemChange::devices);
    return added;
}

void FakeAudioSystem::remove(const std::string& id, bool quietly)
{
    const auto state = impl->find(id);
    if (state == nullptr)
        return;
    {
        const std::lock_guard hold(state->lock);
        state->listed = false;
        state->quiet = true;
    }
    if (!quietly)
        if (const auto stream = state->current())
            if (auto* client = stream->client.load())
                client->lost(AudioLoss::unplugged, 0);
    impl->tell(AudioSystemChange::devices);
}

FakeAudioDevice* FakeAudioSystem::device(const std::string& id)
{
    const auto state = impl->find(id, false);
    return state != nullptr ? state->owner : nullptr;
}

void FakeAudioSystem::setDefault(AudioDirection direction, const std::string& id, bool tell)
{
    {
        const std::lock_guard hold(impl->lock);
        (direction == AudioDirection::input ? impl->defaultInput : impl->defaultOutput) = id;
    }
    if (tell)
        impl->tell(AudioSystemChange::defaults);
}

void FakeAudioSystem::notify(AudioSystemChange change, int times, int threads)
{
    if (threads <= 1)
    {
        for (int i = 0; i < times; ++i)
            impl->tell(change);
        return;
    }
    std::vector<std::thread> senders;
    for (int t = 0; t < threads; ++t)
        senders.emplace_back([this, change, times] {
            for (int i = 0; i < times; ++i)
                impl->tell(change);
        });
    for (auto& sender : senders)
        sender.join();
}

void FakeAudioSystem::replay(const std::vector<std::string>& lines, double speed)
{
    const auto began = std::chrono::steady_clock::now();
    for (const auto& text : lines)
    {
        const auto value = json::tryParse(text);
        if (!value)
            continue;
        const auto& line = *value;
        const auto seconds = line["seconds"].numberOr(0.0) / std::max(speed, 1e-6);
        std::this_thread::sleep_until(began + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                                  std::chrono::duration<double>(seconds)));

        const auto event = line["event"].stringOr("");
        const auto id = line["device"].stringOr("");
        if (event == "devices")
        {
            // The devices as recorded: each found again by its identifier or made, and those gone unlisted quietly.
            std::vector<std::string> ids;
            if (const auto* listed = line["devices"].ifArray())
                for (const auto& recorded : *listed)
                {
                    auto info = devices::detail::deviceFrom(recorded);
                    fillDefaults(info);
                    ids.push_back(info.id);
                    if (const auto state = impl->find(info.id, false))
                    {
                        const std::lock_guard hold(state->lock);
                        state->info = std::move(info);
                        state->listed = true;
                        state->quiet = false;
                        continue;
                    }
                    auto state = std::make_shared<FakeAudioDevice::State>();
                    state->system = impl.get();
                    state->info = std::move(info);
                    auto device = std::make_unique<FakeAudioDevice>(state);
                    const std::lock_guard hold(impl->lock);
                    impl->devices.push_back(std::move(device));
                    impl->states.push_back(std::move(state));
                }
            {
                const std::lock_guard hold(impl->lock);
                for (auto& state : impl->states)
                {
                    const std::lock_guard device(state->lock);
                    if (std::find(ids.begin(), ids.end(), state->info.id) == ids.end())
                    {
                        state->listed = false;
                        state->quiet = true;
                    }
                }
                impl->defaultInput = line["defaultInput"].stringOr("");
                impl->defaultOutput = line["defaultOutput"].stringOr("");
            }
            impl->muted = line["inputMuted"].boolOr(false);
            impl->tell(AudioSystemChange::devices);
        }
        else if (event == "notice")
        {
            AudioSystemChange change{};
            if (devices::detail::changeNamed(line["change"].stringOr(""), change))
                impl->tell(change);
        }
        else if (auto* device = this->device(id))
        {
            AudioLoss why{};
            if (event == "lost" && devices::detail::lossNamed(line["why"].stringOr(""), why))
                device->lose(why, line["code"].intOr(0));
            else if (event == "changed")
                device->changed();
            else if (event == "reopen")
                device->askToReopen();
        }
    }
}

void FakeAudioSystem::setInputMuted(bool muted)
{
    impl->muted = muted;
    impl->tell(AudioSystemChange::inputMute);
}

bool FakeAudioSystem::inputMuted()
{
    return impl->muted.load();
}

void FakeAudioSystem::restartService()
{
    std::vector<std::shared_ptr<FakeAudioDevice::State>> states;
    {
        const std::lock_guard hold(impl->lock);
        states = impl->states;
    }
    for (auto& state : states)
        if (const auto stream = state->current())
            if (auto* client = stream->client.load())
                client->lost(AudioLoss::serviceRestarted, 0);
    impl->tell(AudioSystemChange::serviceRestarted);
}

void FakeAudioSystem::advance(std::chrono::nanoseconds by)
{
    impl->clock += by.count();
}

int FakeAudioSystem::describes() const noexcept
{
    return impl->describes.load();
}

int FakeAudioSystem::opens() const noexcept
{
    return impl->opens.load();
}

int FakeAudioSystem::sets() const noexcept
{
    return impl->sets.load();
}

std::string FakeAudioSystem::name() const
{
    return impl->name;
}

std::int64_t FakeAudioSystem::now() const noexcept
{
    return impl->clock.load();
}

std::vector<std::string> FakeAudioSystem::deviceIds()
{
    std::vector<std::string> ids;
    const std::lock_guard hold(impl->lock);
    for (auto& state : impl->states)
    {
        const std::lock_guard device(state->lock);
        if (state->listed)
            ids.push_back(state->info.id);
    }
    return ids;
}

AudioDeviceInfo FakeAudioSystem::describe(const std::string& id)
{
    ++impl->describes;
    const auto state = impl->find(id);
    if (state == nullptr)
        return {.id = id, .answering = false, .problem = "The device isn't there."};

    while (state->owner->hangDescribe.load())
        std::this_thread::sleep_for(1ms);

    const std::lock_guard hold(state->lock);
    if (state->owner->failDescribe.load())
        return {.id = id, .name = state->info.name, .answering = false, .problem = "The device didn't answer."};
    return state->info;
}

std::string FakeAudioSystem::defaultDevice(AudioDirection direction)
{
    const std::lock_guard hold(impl->lock);
    return direction == AudioDirection::input ? impl->defaultInput : impl->defaultOutput;
}

std::unique_ptr<AudioSystemStream> FakeAudioSystem::open(const std::string& id, const AudioStreamRequest& request,
                                                         AudioError& error)
{
    ++impl->opens;
    const auto state = impl->find(id);
    if (state == nullptr)
    {
        error = {"The device isn't there.", "open", -1};
        return nullptr;
    }
    ++state->opens;
    if (state->owner->failOpen.load())
    {
        error = {"The device didn't open.", "open", -1};
        return nullptr;
    }

    auto stream = std::make_shared<FakeStreamState>();
    {
        const std::lock_guard hold(state->lock);
        auto& info = state->info;
        state->lastRequest = request;

        for (const auto channel : request.inputChannels)
            if (channel < 0 || channel >= info.inputs())
            {
                error = {"The device has no input " + std::to_string(channel + 1) + ".", "open", -1};
                return nullptr;
            }
        for (const auto channel : request.outputChannels)
            if (channel < 0 || channel >= info.outputs())
            {
                error = {"The device has no output " + std::to_string(channel + 1) + ".", "open", -1};
                return nullptr;
            }

        if (request.sampleRate > 0.0)
        {
            if (state->owner->failWhenSetToCurrent.load() && request.sampleRate == info.currentRate)
            {
                error = {"The device failed when set to the rate it runs at.", "set rate", -1};
                return nullptr;
            }
            if (std::find(info.sampleRates.begin(), info.sampleRates.end(), request.sampleRate) ==
                info.sampleRates.end())
            {
                error = {"The device can't run at that rate.", "set rate", -1};
                return nullptr;
            }
            if (request.sampleRate != info.currentRate)
            {
                ++impl->sets;
                info.currentRate = request.sampleRate;
            }
        }
        if (request.blockSize > 0)
        {
            if (state->owner->failWhenSetToCurrent.load() && request.blockSize == info.currentBlock)
            {
                error = {"The device failed when set to the block size it runs at.", "set block size", -1};
                return nullptr;
            }
            if (std::find(info.blockSizes.begin(), info.blockSizes.end(), request.blockSize) ==
                info.blockSizes.end())
            {
                error = {"The device can't take that block size.", "set block size", -1};
                return nullptr;
            }
            if (request.blockSize != info.currentBlock)
            {
                ++impl->sets;
                info.currentBlock = request.blockSize;
            }
        }

        stream->device = state;
        stream->system = impl.get();
        stream->request = request;
        stream->opened.sampleRate = info.currentRate;
        stream->opened.maxBlock = info.currentBlock;
        stream->opened.inputChannels = static_cast<int>(request.inputChannels.size());
        stream->opened.outputChannels = static_cast<int>(request.outputChannels.size());
        stream->opened.inputLatency = info.currentBlock;
        stream->opened.outputLatency = info.currentBlock;
        stream->opened.priority = "fake";
    }

    // Room for the largest cycle a test gives, three times the block size and more.
    stream->capacity = std::max<std::size_t>(16384, 4 * static_cast<std::size_t>(stream->opened.maxBlock));
    stream->inputs.assign(request.inputChannels.size(), std::vector<float>(stream->capacity));
    stream->outputs.assign(request.outputChannels.size(), std::vector<float>(stream->capacity));
    for (auto& channel : stream->inputs)
        stream->inputPointers.push_back(channel.data());
    for (auto& channel : stream->outputs)
        stream->outputPointers.push_back(channel.data());

    {
        const std::lock_guard hold(state->streamLock);
        state->stream = stream;
    }
    return std::make_unique<FakeStream>(stream);
}

void FakeAudioSystem::addListener(AudioSystemListener& listener)
{
    const std::lock_guard hold(impl->listenerLock);
    impl->listeners.push_back(&listener);
}

void FakeAudioSystem::removeListener(AudioSystemListener& listener)
{
    std::unique_lock hold(impl->listenerLock);
    std::erase(impl->listeners, &listener);
    impl->listenersIdle.wait(hold, [this] { return impl->callsUnderWay == 0; });
}

}
