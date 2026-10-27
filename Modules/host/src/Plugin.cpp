#include "symbolengine/host/Plugin.h"

#include "ProcessState.h"

#include "symbolengine/core/LockFreeFifo.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <exception>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace symbolengine
{

namespace
{

// Set while the calling thread is inside a plugin's process call, so a notice the plugin sends from there is only
// recorded, never posted, which would allocate on the audio thread.
thread_local bool insideProcess = false;

}

struct PluginHost::Shared : std::enable_shared_from_this<PluginHost::Shared>
{
    Options options;

    std::mutex lock;
    std::deque<std::pair<std::weak_ptr<Plugin::Inner>, PluginFormat*>> queue;
    bool making = false;

    // Plugins with notices sent from the audio thread, which a thread of the host's own hands to the main thread.
    std::vector<std::weak_ptr<Plugin::Inner>> plugins;
    std::atomic<bool> audioNotices{false}, stopping{false};
    std::thread watcher;

    void misuse(const std::string& what) const
    {
        if (options.misuse != nullptr)
        {
            options.misuse(what);
            return;
        }

#if !defined(NDEBUG)
        std::fprintf(stderr, "SymbolEngine: a hosted plugin was misused: %s\n", what.c_str());
        std::abort();
#endif
    }

    void post(std::function<void()> task) const { options.post(std::move(task)); }

    bool onMainThread() const { return options.onMainThread(); }

    void enqueue(std::weak_ptr<Plugin::Inner> plugin, PluginFormat& format);
    void makeNext();
    void sweepNotices();
};

struct Plugin::Inner : PluginFormatClient, std::enable_shared_from_this<Plugin::Inner>
{
    std::shared_ptr<PluginHost::Shared> host;
    PluginDescription description;
    Plugin* owner = nullptr;

    // The main thread's.
    std::unique_ptr<PluginFormatInstance> instance;
    PluginError problem;
    PluginLayout layout{};
    PluginSetup setup{};
    int latency = 0, tail = 0;
    int busy = 0;
    bool activated = false;   // whether the plugin itself is active, which a failed one may still be
    std::vector<std::function<void()>> waiting;

    // Its parameters and their values as the host knows them, and changes its processing hasn't yet heard, kept while
    // nothing plays it.
    std::vector<PluginParameter> parameterList;
    std::map<std::uint32_t, double> values;
    std::vector<PluginParameterChange> unheard;

    // Shared with the audio thread.
    std::atomic<State> state{State::making};
    std::atomic<bool> accepting{false}, inProcess{false}, processing{false};
    PluginFormatInstance* audioInstance = nullptr;
    std::vector<std::vector<float>> inputs, outputs;
    std::vector<const float*> inputPointers;
    std::vector<float*> outputPointers;

    // Changes the main thread sets for the audio thread to give the plugin with its next block, never locked or waited
    // on, and the count of blocks played, by which the main thread knows a block has carried them.
    static constexpr std::size_t changesPerBlock = 1024;
    LockFreeFifo<PluginParameterChange> toAudio{changesPerBlock};
    std::vector<PluginParameterChange> blockChanges = std::vector<PluginParameterChange>(changesPerBlock);
    std::atomic<std::uint64_t> blocksPlayed{0};

    // Notices, recorded from any thread and acted on from the main thread.
    std::atomic<std::uint32_t> pending{0};
    std::atomic<bool> noticeScheduled{false};

    void notice(PluginNotice what) noexcept override
    {
        pending.fetch_or(static_cast<std::uint32_t>(what));

        if (insideProcess)
        {
            host->audioNotices = true;
            return;
        }

        scheduleNotices();
    }

    void scheduleNotices()
    {
        if (!noticeScheduled.exchange(true))
            host->post([weak = weak_from_this()] {
                if (const auto inner = weak.lock())
                    inner->handleNotices();
            });
    }

    void tell(PluginChange change)
    {
        host->post([weak = weak_from_this(), change] {
            if (const auto inner = weak.lock();
                inner != nullptr && inner->owner != nullptr && inner->owner->onChange != nullptr)
                inner->owner->onChange(change);
        });
    }

    // A call into the plugin on the main thread: marked busy while it runs, so what would overlap it waits; what it
    // changes of the whole process put back; and an exception from it failing the plugin instead of reaching the
    // program.
    template <typename Call>
    bool call(const char* step, Call&& body)
    {
        const bool outermost = busy++ == 0;
        const auto before = outermost ? ProcessState::now() : ProcessState{};
        bool fine = true;

        try
        {
            body();
        }
        catch (const std::exception& error)
        {
            fail(step, std::string("It failed with its own error: ") + error.what());
            fine = false;
        }
        catch (...)
        {
            fail(step, "It failed with an error of its own.");
            fine = false;
        }

        if (outermost && before.putBack())
            tell(PluginChange::processState);

        if (--busy == 0 && !waiting.empty())
        {
            auto ready = std::move(waiting);
            waiting.clear();

            for (auto& then : ready)
                host->post([weak = weak_from_this(), then = std::move(then)] {
                    if (const auto alive = weak.lock())
                        then();
                });
        }

        return fine;
    }

    void fail(const char* step, std::string message)
    {
        accepting = false;
        problem = {std::move(message), step, 0};
        state = State::failed;
        tell(PluginChange::failed);
    }

    // Whether a call the program asks for can be made now: on the main thread, the plugin made and not failed, and no
    // other call into it under way.
    bool usable(const char* what, bool needsActive = false)
    {
        if (!host->onMainThread())
        {
            host->misuse(std::string(what) + " was called on a thread other than the main thread");
            return false;
        }

        if (state == State::making)
        {
            host->misuse(std::string(what) + " was called before the plugin was made");
            return false;
        }

        if (needsActive && state == State::inactive)
        {
            host->misuse(std::string(what) + " was called while the plugin wasn't prepared");
            return false;
        }

        return state != State::failed && busy == 0 && instance != nullptr;
    }

    // Takes the plugin out of processing: the audio thread passes the audio by from its next block, and the main
    // thread waits, within the limit, for it to leave a process call it is in. One that doesn't leave in time is left
    // out for good.
    bool hold()
    {
        accepting = false;
        const auto until = std::chrono::steady_clock::now() + host->options.audioLimit;

        while (inProcess)
        {
            if (std::chrono::steady_clock::now() > until)
            {
                problem = {"It didn't return from processing audio within " +
                               std::to_string(host->options.audioLimit.count()) + " ms.",
                           "process", 0};
                state = State::failed;
                tell(PluginChange::stuck);
                return false;
            }

            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }

        // What the audio thread didn't take waits for the next block or a flush; with it out, this thread reads it.
        for (PluginParameterChange change; toAudio.pop(change);)
            keepUnheard(change);

        if (processing.exchange(false))
            return call("stopProcessing", [this] { instance->stopProcessing(); });

        return true;
    }

    void keepUnheard(const PluginParameterChange& change)
    {
        const auto same = std::find_if(unheard.begin(), unheard.end(),
                                       [&change](const PluginParameterChange& kept) { return kept.id == change.id; });

        if (same != unheard.end())
            same->value = change.value;
        else
            unheard.push_back({change.id, 0, change.value});
    }

    // Hands the plugin's processing what it hasn't heard, while it is active and nothing plays it.
    bool flushUnheard()
    {
        if (unheard.empty() || state != State::active)
            return true;

        auto changes = std::move(unheard);
        unheard.clear();
        return call("flush", [&] { instance->flush(changes.data(), static_cast<int>(changes.size())); });
    }

    void readParameters()
    {
        call("parameters", [this] {
            parameterList = instance->parameters();
            values.clear();

            for (const auto& parameter : parameterList)
                values[parameter.id] = instance->parameterValue(parameter.id);
        });
    }

    bool deactivate()
    {
        const bool fine = call("deactivate", [this] { instance->deactivate(); });
        activated = false;
        return fine;
    }

    void open()
    {
        audioInstance = instance.get();
        accepting = state == State::active;
    }

    bool activate()
    {
        PluginError error;
        bool done = false;

        if (!call("prepare", [&] { done = instance->activate(setup, error); }))
            return false;

        activated = done;

        if (!done)
        {
            problem = error ? error : PluginError{"It couldn't be prepared.", "prepare", 0};
            problem.step = "prepare";
            state = State::inactive;
            return false;
        }

        const auto channels = [](int count, std::vector<std::vector<float>>& buffers, int frames) {
            buffers.assign(static_cast<std::size_t>(std::max(0, count)),
                           std::vector<float>(static_cast<std::size_t>(std::max(1, frames)), 0.0f));
        };

        channels(layout.inputs, inputs, setup.maximumFrames);
        channels(layout.outputs, outputs, setup.maximumFrames);
        inputPointers.clear();
        outputPointers.clear();

        for (auto& buffer : inputs)
            inputPointers.push_back(buffer.data());

        for (auto& buffer : outputs)
            outputPointers.push_back(buffer.data());

        state = State::active;
        return call("latency", [this] {
            latency = std::max(0, instance->latency());
            tail = std::max(0, instance->tail());
        });
    }

    // Stereo where it takes it, else mono in and stereo out, else mono, else what it settles on with audio out.
    bool settleLayout()
    {
        PluginLayout settled{};

        for (const PluginLayout wanted : {PluginLayout{2, 2}, PluginLayout{1, 2}, PluginLayout{1, 1}})
        {
            if (!call("arrange", [&] { settled = instance->arrange(wanted); }))
                return false;

            if (settled == wanted)
                break;
        }

        layout = settled;

        if (layout.outputs <= 0)
        {
            problem = {"It gives no audio out.", "arrange", 0};
            state = State::failed;
            return false;
        }

        return true;
    }

    void made(std::unique_ptr<PluginFormatInstance> madeInstance, PluginError error)
    {
        if (owner == nullptr)
        {
            // Let go of before it was made: taken apart at once, here on the main thread.
            const auto ignored = call("destroy", [&] { madeInstance.reset(); });
            static_cast<void>(ignored);
            return;
        }

        if (madeInstance == nullptr)
        {
            problem = error ? error : PluginError{"It couldn't be made.", "make", 0};
            state = State::failed;
        }
        else
        {
            instance = std::move(madeInstance);
            state = State::inactive;

            if (settleLayout())
                readParameters();
        }

        tell(PluginChange::made);

        if (pending != 0)
            scheduleNotices();
    }

    void handleNotices()
    {
        noticeScheduled = false;

        if (owner == nullptr || state == State::making)
            return;

        if (state == State::failed)
        {
            pending = 0;
            return;
        }

        // Never inside another call into the plugin: once it has returned.
        if (busy > 0)
        {
            noticeScheduled = true;
            waiting.push_back([this] { handleNotices(); });
            return;
        }

        const auto what = static_cast<PluginNotice>(pending.exchange(0));

        if (has(what, PluginNotice::buses) || has(what, PluginNotice::latency))
        {
            const bool wasActive = state == State::active;
            const auto before = std::make_pair(latency, tail);
            const auto layoutBefore = layout;

            if (wasActive && (!hold() || !deactivate()))
                return;

            if (state == State::failed)
                return;

            state = State::inactive;

            if (has(what, PluginNotice::buses) && !settleLayout())
            {
                tell(PluginChange::failed);
                return;
            }

            if (wasActive && !activate())
            {
                tell(PluginChange::failed);
                return;
            }

            open();

            if (has(what, PluginNotice::buses) && layout != layoutBefore)
                tell(PluginChange::buses);

            if (has(what, PluginNotice::latency) || std::make_pair(latency, tail) != before)
                tell(PluginChange::latency);
        }

        if (has(what, PluginNotice::parameters) || has(what, PluginNotice::values))
            readParameters();

        if (has(what, PluginNotice::parameters))
            tell(PluginChange::parameters);

        if (has(what, PluginNotice::values))
            tell(PluginChange::values);

        if (has(what, PluginNotice::state))
            tell(PluginChange::state);

        if (has(what, PluginNotice::reload))
            tell(PluginChange::reload);
    }

    // Taken down in order on the main thread: processing stopped, reset, deactivated, taken apart.
    void takeDown()
    {
        owner = nullptr;

        if (instance == nullptr)
            return;

        if (state == State::failed && inProcess)
        {
            // Still inside a process call it never returned from: its memory is kept, never freed under that call.
            static_cast<void>(instance.release());
            return;
        }

        if (!hold())
        {
            static_cast<void>(instance.release());
            return;
        }

        if (activated)
        {
            call("reset", [this] { instance->reset(); });
            deactivate();
        }

        call("destroy", [this] { instance.reset(); });
        state = State::failed;
    }
};

void PluginHost::Shared::enqueue(std::weak_ptr<Plugin::Inner> plugin, PluginFormat& format)
{
    bool start = false;
    {
        const std::lock_guard held(lock);
        queue.emplace_back(std::move(plugin), &format);
        plugins.push_back(queue.back().first);
        start = !making;
        making = true;
    }

    if (start)
        post([self = shared_from_this()] { self->makeNext(); });
}

// Makes the next plugin asked for, one per turn of the run loop: the next is started only once this one says it is
// made, on a later turn.
void PluginHost::Shared::makeNext()
{
    std::shared_ptr<Plugin::Inner> plugin;
    PluginFormat* format = nullptr;

    {
        const std::lock_guard held(lock);

        while (plugin == nullptr && !queue.empty())
        {
            plugin = queue.front().first.lock();
            format = queue.front().second;
            queue.pop_front();
        }

        if (plugin == nullptr)
        {
            making = false;
            return;
        }
    }

    const auto next = [self = shared_from_this()] { self->post([self] { self->makeNext(); }); };
    auto done = [plugin, next](std::unique_ptr<PluginFormatInstance> made, PluginError error) {
        plugin->made(std::move(made), std::move(error));
        next();
    };

    if (plugin->owner == nullptr)
    {
        next();
        return;
    }

    const bool asked = plugin->call("make", [&] { format->make(plugin->description, *plugin, done); });

    if (!asked)
    {
        plugin->tell(PluginChange::made);
        next();
    }
}

void PluginHost::Shared::sweepNotices()
{
    std::vector<std::shared_ptr<Plugin::Inner>> live;
    {
        const std::lock_guard held(lock);
        std::erase_if(plugins, [](const std::weak_ptr<Plugin::Inner>& plugin) { return plugin.expired(); });

        for (const auto& plugin : plugins)
            if (auto inner = plugin.lock())
                live.push_back(std::move(inner));
    }

    for (const auto& plugin : live)
        if (plugin->pending != 0)
            plugin->scheduleNotices();
}

PluginHost::PluginHost(Options options) : shared(std::make_shared<Shared>())
{
    shared->options = std::move(options);

    // Notices a plugin sends from inside its process call are only recorded there; this thread passes them to the main
    // thread.
    shared->watcher = std::thread([weak = std::weak_ptr(shared)] {
        for (;;)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            const auto host = weak.lock();

            if (host == nullptr || host->stopping)
                return;

            if (host->audioNotices.exchange(false))
                host->post([host] { host->sweepNotices(); });
        }
    });
}

PluginHost::~PluginHost()
{
    shared->stopping = true;

    if (shared->watcher.joinable())
        shared->watcher.join();
}

std::shared_ptr<Plugin> PluginHost::make(PluginFormat& format, const PluginDescription& description)
{
    auto inner = std::make_shared<Plugin::Inner>();
    inner->host = shared;
    inner->description = description;

    // The last hold, let go of on any thread, hands its taking down to the main thread, which waits until no call into
    // the plugin is under way.
    std::shared_ptr<Plugin> plugin(new Plugin(inner), [host = shared](Plugin* gone) {
        const auto takeDown = [gone] {
            auto& held = *gone->inner;

            if (held.busy > 0)
                held.waiting.push_back([gone] { delete gone; });
            else
                delete gone;
        };

        if (host->onMainThread())
            takeDown();
        else
            host->post(takeDown);
    });

    inner->owner = plugin.get();
    shared->enqueue(inner, format);
    return plugin;
}

Plugin::Plugin(std::shared_ptr<Inner> made) : inner(std::move(made)) {}

Plugin::~Plugin()
{
    inner->takeDown();
}

Plugin::State Plugin::state() const
{
    return inner->state;
}

const PluginDescription& Plugin::description() const
{
    return inner->description;
}

PluginError Plugin::problem() const
{
    return inner->problem;
}

PluginLayout Plugin::layout() const
{
    return inner->layout;
}

bool Plugin::prepare(const PluginSetup& setup)
{
    if (!inner->usable("prepare"))
        return false;

    if (inner->state == State::active && (!inner->hold() || !inner->deactivate()))
        return false;

    if (inner->state == State::failed)
        return false;

    inner->state = State::inactive;
    inner->setup = setup;

    // Values set while it wasn't prepared reach its processing before any block.
    if (!inner->activate() || !inner->flushUnheard())
        return false;

    inner->open();
    return true;
}

void Plugin::release()
{
    if (!inner->usable("release") || inner->state != State::active)
        return;

    if (inner->hold() && inner->deactivate() && inner->state != State::failed)
        inner->state = State::inactive;
}

bool Plugin::reset()
{
    if (!inner->usable("reset", true))
        return false;

    if (!inner->hold() || !inner->call("reset", [this] { inner->instance->reset(); }))
        return false;

    inner->open();
    return inner->state == State::active;
}

int Plugin::latency() const
{
    return inner->latency;
}

int Plugin::tail() const
{
    return inner->tail;
}

std::optional<std::string> Plugin::saveState(PluginStateUse use)
{
    if (!inner->usable("saveState"))
        return std::nullopt;

    // Every value set reaches its processing first: while a device plays it, the next block carries them, and the
    // state is taken after that block; otherwise, or if no block comes, they are handed over with nothing playing.
    auto& held = *inner;

    if (held.accepting && held.toAudio.readable() > 0)
    {
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
        const auto before = held.blocksPlayed.load();

        while (held.accepting && std::chrono::steady_clock::now() < until &&
               (held.toAudio.readable() > 0 || held.blocksPlayed.load() <= before))
            std::this_thread::sleep_for(std::chrono::microseconds(200));
    }

    if (held.toAudio.readable() > 0 || !held.unheard.empty())
    {
        const bool wasAccepting = held.accepting;

        if (!held.hold() || !held.flushUnheard())
            return std::nullopt;

        if (wasAccepting)
            held.open();
    }

    std::optional<std::string> state;
    PluginError error;

    if (!inner->call("saveState", [&] { state = inner->instance->state(use, error); }))
        return std::nullopt;

    if (!state.has_value())
        inner->problem = error ? error : PluginError{"It gave no state.", "saveState", 0};

    return state;
}

bool Plugin::loadState(std::string_view state, PluginStateUse use)
{
    if (!inner->usable("loadState"))
        return false;

    const bool active = inner->state == State::active;

    if (active && !inner->hold())
        return false;

    PluginError error;
    bool loaded = false;

    if (!inner->call("loadState", [&] { loaded = inner->instance->setState(state, use, error); }))
        return false;

    if (!loaded)
        inner->problem = error ? error : PluginError{"It didn't take the state.", "loadState", 0};

    // A plugin's latency can follow its state.
    if (active)
    {
        const auto before = std::make_pair(inner->latency, inner->tail);
        inner->call("latency", [this] {
            inner->latency = std::max(0, inner->instance->latency());
            inner->tail = std::max(0, inner->instance->tail());
        });

        if (std::make_pair(inner->latency, inner->tail) != before)
            inner->tell(PluginChange::latency);

        inner->open();
    }

    return loaded && inner->state != State::failed;
}

std::vector<PluginParameter> Plugin::parameters() const
{
    return inner->parameterList;
}

double Plugin::parameter(std::uint32_t id) const
{
    const auto found = inner->values.find(id);
    return found != inner->values.end() ? found->second : 0.0;
}

bool Plugin::setParameter(std::uint32_t id, double value)
{
    if (!inner->usable("setParameter"))
        return false;

    const auto& list = inner->parameterList;
    const auto parameter =
        std::find_if(list.begin(), list.end(), [id](const PluginParameter& one) { return one.id == id; });

    if (parameter == list.end() || parameter->readOnly)
        return false;

    value = std::clamp(value, 0.0, 1.0);
    inner->values[id] = value;

    if (!inner->call("setParameter", [&] { inner->instance->showParameterValue(id, value); }))
        return false;

    // To the audio thread while it may be playing the plugin; kept for a flush while nothing does.
    const PluginParameterChange change{id, 0, value};

    if (!inner->accepting || !inner->toAudio.push(change))
        inner->keepUnheard(change);

    return true;
}

std::string Plugin::parameterText(std::uint32_t id, double value)
{
    std::string text;

    if (inner->usable("parameterText"))
        inner->call("parameterText", [&] { text = inner->instance->parameterText(id, value); });

    return text;
}

bool Plugin::isBusy() const
{
    return inner->busy > 0;
}

void Plugin::whenFree(std::function<void()> then)
{
    if (!inner->host->onMainThread())
    {
        inner->host->misuse("whenFree was called on a thread other than the main thread");
        return;
    }

    if (inner->busy > 0)
        inner->waiting.push_back(std::move(then));
    else
        then();
}

void Plugin::process(float* const* channels, int channelCount, int frames) noexcept
{
    auto& held = *inner;

    if (channelCount <= 0 || frames <= 0)
        return;

    // Entered before accepting is read, as the main thread leaves accepting before reading inProcess, so the two never
    // both go ahead.
    held.inProcess = true;

    if (!held.accepting)
    {
        held.inProcess = false;
        return;
    }

    insideProcess = true;
    auto* plugin = held.audioInstance;
    const auto modes = floatingPointModes();

    if (!held.processing.exchange(true))
        plugin->startProcessing();

    const int ins = static_cast<int>(held.inputs.size());
    const int outs = static_cast<int>(held.outputs.size());
    const int most = std::max(1, held.setup.maximumFrames);

    for (int offset = 0; offset < frames; offset += most)
    {
        const int count = std::min(most, frames - offset);

        for (int c = 0; c < ins; ++c)
            std::copy_n(channels[std::min(c, channelCount - 1)] + offset, count,
                        held.inputs[static_cast<std::size_t>(c)].begin());

        for (auto& output : held.outputs)
            std::fill_n(output.begin(), count, 0.0f);

        // The changes the main thread set since the last block, at the start of this one.
        const auto changes = offset == 0 ? held.toAudio.read(held.blockChanges.data(), held.blockChanges.size()) : 0;
        const PluginBlock block{held.inputPointers.data(), held.outputPointers.data(), ins, outs, count,
                                held.blockChanges.data(),  static_cast<int>(changes)};
        const bool fine = plugin->process(block);

        // A plugin's changes to the thread's floating-point modes go no further than its own call.
        if (floatingPointModes() != modes)
            setFloatingPointModes(modes);

        // A block it failed is heard as it came.
        if (!fine || outs == 0)
            continue;

        for (int c = 0; c < channelCount; ++c)
            std::copy_n(held.outputs[static_cast<std::size_t>(std::min(c, outs - 1))].begin(), count,
                        channels[c] + offset);
    }

    ++held.blocksPlayed;
    insideProcess = false;
    held.inProcess = false;
}

}
