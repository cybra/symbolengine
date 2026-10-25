#include "symbolengine/devices/AudioDevices.h"

#include "EventQueue.h"
#include "Systems.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <future>
#include <map>
#include <mutex>
#include <set>
#include <thread>

// The lists are read on a thread of the devices' own, never the program's: a device's description is asked for on a
// thread of its own and waited for a bounded time, so one device whose calls never return is listed as not answering
// and the rest are listed, its thread left to end when the device lets it. A system's notices only mark it to be read
// again once they have settled, and what changed is told by comparing the lists, so a burst of notices for one change
// is one event.
namespace symbolengine
{

namespace
{
using SteadyClock = std::chrono::steady_clock;

struct Listed
{
    std::vector<AudioDeviceInfo> devices;
    std::string defaultInput, defaultOutput;
};

bool sameList(const std::vector<AudioDeviceInfo>& a, const std::vector<AudioDeviceInfo>& b)
{
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i].id != b[i].id || a[i].name != b[i].name || a[i].answering != b[i].answering ||
            a[i].inputNames != b[i].inputNames || a[i].outputNames != b[i].outputNames ||
            a[i].currentRate != b[i].currentRate || a[i].currentBlock != b[i].currentBlock || a[i].unread != b[i].unread)
            return false;
    return true;
}
}

struct AudioDevices::Impl
{
    // Each system with what was last read of it, and a listener that marks it to be read again.
    struct Watched final : AudioSystemListener
    {
        Impl* owner = nullptr;
        std::shared_ptr<AudioSystem> system;
        Listed listed;
        std::atomic<bool> pending{false};
        std::atomic<std::int64_t> noticedAt{0};

        void systemChanged(AudioSystemChange change) noexcept override
        {
            if (change != AudioSystemChange::devices && change != AudioSystemChange::defaults &&
                change != AudioSystemChange::serviceRestarted)
                return;
            noticedAt = SteadyClock::now().time_since_epoch().count();
            pending = true;
            owner->nudge();
        }
    };

    Options options;
    std::shared_ptr<devices::detail::EventQueue> queue;
    std::vector<std::unique_ptr<Watched>> watched;

    mutable std::mutex lock;
    std::condition_variable wake;
    bool quitting = false;
    bool nudged = false;
    std::thread lister;

    // The devices open, one stream to a device, shared with the streams so each can say it closed.
    struct Open
    {
        std::mutex lock;
        std::set<std::pair<std::string, std::string>> devices;
    };
    std::shared_ptr<Open> open = std::make_shared<Open>();

    void nudge()
    {
        {
            const std::lock_guard hold(lock);
            nudged = true;
        }
        wake.notify_one();
    }

    Listed read(AudioSystem& system, const std::shared_ptr<AudioSystem>& keep) const
    {
        Listed now;
        const auto ids = system.deviceIds();

        // Every device asked at once, each on a thread of its own, with one deadline for all.
        std::vector<std::future<AudioDeviceInfo>> asked;
        for (const auto& id : ids)
        {
            std::packaged_task<AudioDeviceInfo()> task([keep, id] { return keep->describe(id); });
            asked.push_back(task.get_future());
            std::thread(std::move(task)).detach();
        }
        const auto until = SteadyClock::now() + options.describeTimeout;
        for (std::size_t i = 0; i < ids.size(); ++i)
        {
            if (asked[i].wait_until(until) == std::future_status::ready)
                now.devices.push_back(asked[i].get());
            else
            {
                AudioDeviceInfo stuck;
                stuck.id = ids[i];
                stuck.answering = false;
                stuck.problem = "The device isn't answering.";
                now.devices.push_back(std::move(stuck));
            }
        }

        // Each device's place among those of the same name, in the system's order.
        std::map<std::string, int> seen;
        for (auto& device : now.devices)
            if (!device.name.empty())
                device.sameNameIndex = seen[device.name]++;

        now.defaultInput = system.defaultDevice(AudioDirection::input);
        now.defaultOutput = system.defaultDevice(AudioDirection::output);
        return now;
    }

    void refresh(Watched& w, bool tellChanges)
    {
        auto now = read(*w.system, w.system);
        Listed before;
        {
            const std::lock_guard hold(lock);
            before = w.listed;
            w.listed = now;
        }
        if (!tellChanges)
            return;

        const auto name = w.system->name();
        if (!sameList(before.devices, now.devices))
            tell({.kind = AudioEvent::Kind::devicesChanged, .system = name});
        if (before.defaultInput != now.defaultInput)
            tell({.kind = AudioEvent::Kind::defaultChanged,
                  .system = name,
                  .deviceId = now.defaultInput,
                  .direction = AudioDirection::input});
        if (before.defaultOutput != now.defaultOutput)
            tell({.kind = AudioEvent::Kind::defaultChanged,
                  .system = name,
                  .deviceId = now.defaultOutput,
                  .direction = AudioDirection::output});
    }

    void listLoop()
    {
        std::unique_lock hold(lock);
        while (!quitting)
        {
            // Notices still settling are looked at again shortly; otherwise the thread waits for the next.
            const bool settling = std::any_of(watched.begin(), watched.end(), [](auto& w) { return w->pending.load(); });
            wake.wait_for(hold, settling ? std::chrono::milliseconds(5) : std::chrono::milliseconds(500),
                          [&] { return quitting || nudged; });
            nudged = false;
            if (quitting)
                break;
            hold.unlock();

            const auto settle = std::chrono::duration_cast<SteadyClock::duration>(options.settle).count();
            for (auto& w : watched)
                if (w->pending.load() && SteadyClock::now().time_since_epoch().count() - w->noticedAt.load() >= settle)
                {
                    w->pending = false;
                    refresh(*w, true);
                }
            hold.lock();
        }
    }

    void tell(AudioEvent event)
    {
        if (!options.onEvent)
            return;
        auto deliver = [handler = options.onEvent, event = std::move(event)] { handler(event); };
        if (options.post)
            options.post(std::move(deliver));
        else
            queue->post(std::move(deliver));
    }

    Watched* find(const std::string& system) const
    {
        for (const auto& w : watched)
            if (w->system->name() == system)
                return w.get();
        return nullptr;
    }
};

AudioDevices::AudioDevices() : AudioDevices(Options{}) {}

AudioDevices::AudioDevices(Options options) : impl(std::make_shared<Impl>())
{
    impl->options = std::move(options);
    if (!impl->options.post)
    {
        impl->queue = devices::detail::EventQueue::make();
        impl->options.post = [queue = impl->queue](std::function<void()> work) { queue->post(std::move(work)); };
    }
    impl->options.streams.post = impl->options.post;
    impl->options.streams.onEvent = impl->options.onEvent;
    if (impl->options.systems.empty())
        impl->options.systems = devices::detail::systemsOfThisComputer();

    for (auto& system : impl->options.systems)
    {
        auto w = std::make_unique<Impl::Watched>();
        w->owner = impl.get();
        w->system = system;
        impl->watched.push_back(std::move(w));
    }

    // The first lists are read on the lister's own thread, as every later one is, and waited for here.
    std::promise<void> listed;
    auto first = listed.get_future();
    impl->lister = std::thread([raw = impl.get(), &listed] {
        for (auto& w : raw->watched)
        {
            raw->refresh(*w, false);
            w->system->addListener(*w);
        }
        listed.set_value();
        raw->listLoop();
    });
    first.wait();
}

AudioDevices::~AudioDevices()
{
    for (auto& w : impl->watched)
        w->system->removeListener(*w);
    {
        const std::lock_guard hold(impl->lock);
        impl->quitting = true;
    }
    impl->wake.notify_one();
    impl->lister.join();
    if (impl->queue != nullptr)
        impl->queue->stop();
}

std::vector<std::string> AudioDevices::systems() const
{
    std::vector<std::string> names;
    for (const auto& w : impl->watched)
        names.push_back(w->system->name());
    return names;
}

std::vector<AudioDeviceInfo> AudioDevices::devices(const std::string& system) const
{
    const auto* w = impl->find(system);
    if (w == nullptr)
        return {};
    const std::lock_guard hold(impl->lock);
    return w->listed.devices;
}

std::string AudioDevices::defaultDevice(const std::string& system, AudioDirection direction) const
{
    const auto* w = impl->find(system);
    if (w == nullptr)
        return {};
    const std::lock_guard hold(impl->lock);
    return direction == AudioDirection::input ? w->listed.defaultInput : w->listed.defaultOutput;
}

AudioResolution AudioDevices::resolve(const AudioSetup& setup) const
{
    const auto* w = impl->find(setup.system);
    if (w == nullptr && !impl->watched.empty())
        w = impl->watched.front().get();
    if (w == nullptr)
        return setup.resolve({}, {}, {});
    Listed listed;
    {
        const std::lock_guard hold(impl->lock);
        listed = w->listed;
    }
    return setup.resolve(listed.devices, listed.defaultInput, listed.defaultOutput);
}

std::unique_ptr<AudioStream> AudioDevices::open(const std::string& system, const std::string& deviceId,
                                               AudioStreamRequest request, AudioError& error)
{
    const auto* w = impl->find(system);
    if (w == nullptr)
    {
        error = {"There is no audio system called " + system + ".", "open"};
        return nullptr;
    }
    const auto key = std::make_pair(system, deviceId);
    {
        const std::lock_guard hold(impl->open->lock);
        if (!impl->open->devices.insert(key).second)
        {
            error = {"The audio device is already open.", "open"};
            return nullptr;
        }
    }

    // The device the stream is on now, which a stream following the default output changes, under the list's lock.
    auto current = std::make_shared<std::pair<std::string, std::string>>(key);
    auto stream = std::make_unique<AudioStream>(w->system, deviceId, std::move(request), impl->options.streams);
    stream->whenMoving([open = std::weak_ptr<Impl::Open>(impl->open), current](const std::string& to) {
        const auto still = open.lock();
        if (still == nullptr)
            return true;
        const std::lock_guard hold(still->lock);
        const auto next = std::make_pair(current->first, to);
        if (!still->devices.insert(next).second)
            return false;
        still->devices.erase(*current);
        *current = next;
        return true;
    });
    stream->whenClosed([open = std::weak_ptr<Impl::Open>(impl->open), current] {
        if (const auto still = open.lock())
        {
            const std::lock_guard hold(still->lock);
            still->devices.erase(*current);
        }
    });
    return stream;
}

}
