#include "symbolengine/devices/AudioStream.h"

#include "AudioThreadMarker.h"
#include "EventQueue.h"

#include <symbolengine/core/LockFreeFifo.h>
#include <symbolengine/core/NoDenormals.h>
#include <symbolengine/devices/AudioThread.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

// A stream's state belongs to its control thread. The program's calls reach it as commands and wait a bounded time
// for their answers; the system's notifications set flags and wake it; the audio thread reads only what the control
// thread made before letting it in, a published list of callbacks it never changes, and leaves notes in a lock-free
// fifo. Letting the audio thread in and out is two flags in sequential order: it marks itself in a cycle and then
// looks whether it may be, and the control thread forbids it and then waits for it to be out, so one of them always
// sees the other.
namespace symbolengine
{

namespace
{
using SteadyClock = std::chrono::steady_clock;

// The stream whose control thread the calling thread is, so its calls from callbacks are refused, not deadlocked.
thread_local const void* controlThreadOf = nullptr;

std::int64_t steadyNanoseconds()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(SteadyClock::now().time_since_epoch()).count();
}

std::int64_t nanoseconds(std::chrono::milliseconds time)
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(time).count();
}

// What the audio thread found, for the control thread to tell.
struct Note
{
    enum class Kind : std::uint8_t
    {
        dropout,
        silentInput,
        inputSound
    };

    Kind kind = Kind::dropout;
    AudioDirection direction = AudioDirection::input;
    bool reported = true;
    std::int64_t position = 0;
    std::int64_t frames = -1;
};

// The callbacks the audio thread runs, never changed once it may see them.
struct Published
{
    std::uint64_t generation = 0;
    std::vector<AudioCallback*> callbacks;
};

// What one run of the system's stream needs on the audio thread, all made before it starts.
struct Run
{
    double rate = 0.0;
    int maxBlock = 0;
    int inputs = 0;
    int outputs = 0;

    std::vector<float> zeros, discard;
    std::vector<std::vector<float>> scratch;
    std::vector<const float*> inputPointers, chunkInputs;
    std::vector<float*> chunkOutputs, scratchPointers;

    std::int64_t inputPosition = 0, outputPosition = 0;
    std::int64_t expectInput = -1, expectOutput = -1;
    std::int64_t cycles = 0;
    std::int64_t silentFrames = 0, silentLimit = 0;
    bool silentSaid = false;
};

struct Completion
{
    std::mutex lock;
    std::condition_variable wake;
    bool finished = false;
    AudioError error;
};

struct Command
{
    enum class Kind
    {
        start,
        stop,
        change,
        add,
        remove,
        settings
    };

    Kind kind = Kind::start;
    AudioCallback* callback = nullptr;
    AudioStreamRequest request{};
    std::shared_ptr<Completion> done{};
};

bool sameShape(const AudioStreamInfo& a, const AudioStreamInfo& b)
{
    return a.sampleRate == b.sampleRate && a.maxBlock == b.maxBlock && a.inputChannels == b.inputChannels &&
           a.outputChannels == b.outputChannels;
}
}

struct AudioStream::Impl final : AudioSystemClient, AudioSystemListener, std::enable_shared_from_this<Impl>
{
    std::shared_ptr<AudioSystem> system;
    std::string deviceId;
    Options options;
    std::shared_ptr<devices::detail::EventQueue> queue;
    std::function<void()> closed;
    std::function<bool(const std::string&)> moving;

    // Between the program's threads and the control thread.
    std::mutex lock;
    std::condition_variable wake, ended;
    std::deque<Command> commands;
    bool quitting = false;
    bool finished = false;
    std::thread control;
    AudioStreamRequest request;
    AudioStreamInfo info;
    std::string deviceName;
    std::atomic<State> state{State::stopped};

    // The control thread's own.
    std::vector<AudioCallback*> callbacks;
    std::unique_ptr<AudioSystemStream> stream;
    std::vector<std::unique_ptr<Published>> lists;
    std::uint64_t generation = 0;
    std::int64_t startedAt = 0;
    bool stalled = false;
    bool priorityRead = false;
    std::int64_t stalledAt = 0;
    std::int64_t windowAt = 0, windowFrames = 0;
    bool runawaySaid = false;
    std::int64_t sleptAt = 0, wokeAt = -1;

    // The audio thread's, made by the control thread while it is kept out.
    Run run;
    std::atomic<bool> accepting{false}, inCycle{false};
    std::atomic<const Published*> published{nullptr};
    std::atomic<std::uint64_t> seen{0};
    LockFreeFifo<Note> notes{1024};

    // Written by the audio thread, read by anyone.
    std::atomic<std::int64_t> cycles{0}, stretchCycles{0}, stretches{0}, framesThisStretch{0};
    std::atomic<std::int64_t> inputPosition{0}, outputPosition{0}, dropouts{0}, lostFrames{0}, overruns{0};
    std::atomic<std::int64_t> notesLost{0}, lastCycleEnds{-1};
    std::atomic<double> load{0.0};

    // The worst late block since the control thread last told them, written by the audio thread: the count of late
    // blocks it was, when it began, how long it took, its frames and its share of its time, its sequence odd while
    // written. A block counted after the last told starts the record again.
    struct Late
    {
        std::atomic<std::uint32_t> sequence{0};
        std::atomic<std::int64_t> index{0}, began{0}, took{0};
        std::atomic<int> frames{0};
        std::atomic<double> share{0.0};
    };

    Late late;
    std::atomic<std::int64_t> lateTold{0};

    // The control thread's: when it last told late blocks.
    std::int64_t lateToldAt = -1;

    // Set by the system's notifications, from any thread.
    std::atomic<bool> changePending{false}, reopenPending{false}, rebuilding{false};
    std::atomic<std::int64_t> changeAt{0};
    std::atomic<int> lossPending{-1};
    std::atomic<std::int64_t> lossCode{0};
    std::atomic<bool> devicesPending{false}, restartPending{false}, sleepPending{false}, wakePending{false};
    std::atomic<bool> mutePending{false};
    bool mutedSaid = false;

    AudioWorkgroup workgroupNow;

    // The device stopped while every callback is idle, the stream still running to the program.
    bool resting = false;
    std::int64_t idleSince = -1;

    // A stream following the default output: a change of default to look at once it settles, and whether its device
    // has gone, in which case no default to move to loses it.
    std::atomic<bool> defaultsPending{false};
    std::atomic<std::int64_t> defaultsAt{0};
    bool orphaned = false;

    // The program's calls

    AudioError call(Command command)
    {
        if (audioThread::isCurrent())
            return {"A stream can't be started, stopped or changed from its audio thread.", "call"};
        if (controlThreadOf == this)
            return {"A stream can't be started, stopped or changed from its own callbacks.", "call"};
        if (state.load() == State::failed)
            return notAnswering();

        auto done = std::make_shared<Completion>();
        command.done = done;
        {
            const std::lock_guard hold(lock);
            commands.push_back(std::move(command));
        }
        wake.notify_one();

        std::unique_lock waiting(done->lock);
        if (!done->wake.wait_for(waiting, options.callTimeout, [&] { return done->finished; }))
        {
            state = State::failed;
            return notAnswering();
        }
        return done->error;
    }

    AudioError notAnswering() const
    {
        return {"The audio device stopped answering.", "call"};
    }

    void setState(State next)
    {
        auto current = state.load();
        while (current != State::failed && !state.compare_exchange_weak(current, next))
        {
        }
    }

    // The control thread

    void begin()
    {
        system->addListener(*this);
        control = std::thread([self = shared_from_this()] { self->controlLoop(); });
    }

    bool urgent() const
    {
        return lossPending.load() >= 0 || devicesPending.load() || restartPending.load() || sleepPending.load() ||
               wakePending.load() || mutePending.load();
    }

    void controlLoop()
    {
        controlThreadOf = this;
        std::unique_lock hold(lock);
        for (;;)
        {
            const auto now = state.load();
            const bool watching = now == State::running || now == State::sleeping || changePending.load();
            wake.wait_for(hold, watching ? options.watchEvery : std::chrono::milliseconds(250),
                          [&] { return quitting || !commands.empty() || urgent(); });
            if (quitting)
                break;

            std::deque<Command> taken;
            taken.swap(commands);
            hold.unlock();
            for (auto& command : taken)
                finish(command, perform(command));
            handleSystem();
            drainNotes();
            watchIdle();
            watch();
            hold.lock();
        }

        // Commands that came after the quit are answered, and the device closed without a word to the program.
        auto left = std::move(commands);
        hold.unlock();
        for (auto& command : left)
            finish(command, {"The stream is closing.", "call"});
        teardown();
        system->removeListener(*this);
        controlThreadOf = nullptr;

        hold.lock();
        finished = true;
        hold.unlock();
        ended.notify_all();
    }

    static void finish(Command& command, AudioError error)
    {
        {
            const std::lock_guard hold(command.done->lock);
            command.done->error = std::move(error);
            command.done->finished = true;
        }
        command.done->wake.notify_all();
    }

    AudioError perform(const Command& command)
    {
        switch (command.kind)
        {
            case Command::Kind::start: return start();
            case Command::Kind::stop: return stop();
            case Command::Kind::change: return change(command.request);
            case Command::Kind::add: return add(*command.callback);
            case Command::Kind::remove: return remove(*command.callback);
            case Command::Kind::settings:
                return stream != nullptr ? stream->showSettings()
                                         : AudioError{"The audio device's settings open while its stream runs.", "settings"};
        }
        return {};
    }

    AudioError start()
    {
        const auto now = state.load();
        if (now == State::running || now == State::sleeping)
            return {};
        lossPending = -1;
        return openAndBegin(true);
    }

    AudioError stop()
    {
        const bool was = stream != nullptr || resting;
        resting = false;
        teardown();
        wokeAt = -1;
        setState(State::stopped);
        if (was)
            tell({.kind = AudioEvent::Kind::stopped});
        return {};
    }

    AudioError change(const AudioStreamRequest& next)
    {
        AudioStreamRequest before;
        {
            const std::lock_guard hold(lock);
            before = request;
            request = next;
        }
        if (stream == nullptr)
            return {};

        teardown();
        auto error = openAndBegin(true);
        if (!error)
            return {};

        // What was asked for failed; the stream goes on as it was.
        {
            const std::lock_guard hold(lock);
            request = before;
        }
        if (auto again = openAndBegin(true))
            lose(AudioLoss::other, again);
        return error;
    }

    AudioError add(AudioCallback& callback)
    {
        if (std::find(callbacks.begin(), callbacks.end(), &callback) != callbacks.end())
            return {};
        callbacks.push_back(&callback);
        if (stream != nullptr)
        {
            AudioStreamInfo now;
            {
                const std::lock_guard hold(lock);
                now = info;
            }
            callback.prepare(now);
            publish();
        }
        return {};
    }

    AudioError remove(AudioCallback& callback)
    {
        const auto found = std::find(callbacks.begin(), callbacks.end(), &callback);
        if (found == callbacks.end())
            return {};
        callbacks.erase(found);
        if (stream != nullptr)
        {
            publish();
            if (!waitSeen(generation))
            {
                state = State::failed;
                return notAnswering();
            }
            callback.stopped();
        }
        return {};
    }

    // Opens the device as asked, setting nothing it already runs at, prepares the callbacks and starts it.
    AudioError openAndBegin(bool announce)
    {
        resting = false;
        idleSince = -1;
        auto device = system->describe(deviceId);
        if (!device.answering)
            return {device.problem.empty() ? "The audio device isn't answering." : device.problem, "describe"};

        AudioStreamRequest asked;
        {
            const std::lock_guard hold(lock);
            asked = request;
            deviceName = device.name;
        }
        if (asked.sampleRate == device.currentRate)
            asked.sampleRate = 0.0;
        if (asked.blockSize == device.currentBlock)
            asked.blockSize = 0;

        AudioError error;
        auto opened = system->open(deviceId, asked, error);
        if (opened == nullptr)
            return error ? error : AudioError{"The audio device didn't open.", "open"};

        const auto now = opened->info();
        prepareRun(now);
        {
            const std::lock_guard hold(lock);
            info = now;
        }
        for (auto* callback : callbacks)
            callback->prepare(now);
        publish();

        notes.reset();
        stretchCycles = 0;
        framesThisStretch = 0;
        lastCycleEnds = -1;
        inputPosition = 0;
        outputPosition = 0;
        startedAt = system->now();
        windowAt = startedAt;
        windowFrames = 0;
        stalled = false;
        runawaySaid = false;
        priorityRead = false;

        accepting.store(true);
        if (!opened->start(*this, error))
        {
            accepting.store(false);
            if (waitOut())
                for (auto* callback : callbacks)
                    callback->stopped();
            return error ? error : AudioError{"The audio device didn't start.", "start"};
        }

        stream = std::move(opened);
        {
            auto group = stream->workgroup();
            const std::lock_guard hold(lock);
            workgroupNow = std::move(group);
        }
        ++stretches;
        setState(State::running);
        if (announce)
            tell({.kind = AudioEvent::Kind::started, .after = now});

        // An input that starts muted is said once; one unmuted is said only as it changes.
        mutedSaid = false;
        if (run.inputs > 0)
            sayMute();
        return {};
    }

    void sayMute()
    {
        const auto muted = system->inputMuted();
        if (muted != mutedSaid)
            tell({.kind = muted ? AudioEvent::Kind::inputMuted : AudioEvent::Kind::inputUnmuted});
        mutedSaid = muted;
    }

    void prepareRun(const AudioStreamInfo& now)
    {
        auto& r = run;
        r.rate = now.sampleRate;
        r.maxBlock = std::max(1, now.maxBlock);
        r.inputs = now.inputChannels;
        r.outputs = now.outputChannels;
        const auto block = static_cast<std::size_t>(r.maxBlock);
        r.zeros.assign(block, 0.0f);
        r.discard.assign(block, 0.0f);
        r.scratch.assign(static_cast<std::size_t>(r.outputs), std::vector<float>(block, 0.0f));
        r.scratchPointers.clear();
        for (auto& channel : r.scratch)
            r.scratchPointers.push_back(channel.data());
        r.inputPointers.assign(static_cast<std::size_t>(r.inputs), nullptr);
        r.chunkInputs.assign(static_cast<std::size_t>(r.inputs), nullptr);
        r.chunkOutputs.assign(static_cast<std::size_t>(r.outputs), nullptr);
        r.inputPosition = 0;
        r.outputPosition = 0;
        r.expectInput = -1;
        r.expectOutput = -1;
        r.cycles = 0;
        r.silentFrames = 0;
        r.silentSaid = false;
        r.silentLimit = static_cast<std::int64_t>(
            std::llround(r.rate * std::chrono::duration<double>(options.silentInputAfter).count()));

        // Touched once now, so the audio thread never faults on a page of them.
        for (auto& channel : r.scratch)
            std::fill(channel.begin(), channel.end(), 0.0f);
    }

    // Stops the system's stream, if one runs, and tells each callback it stopped once the audio thread is out.
    void teardown()
    {
        if (stream == nullptr)
            return;
        accepting.store(false);
        stream->stop();
        const bool out = waitOut();
        stream.reset();
        // A stream that failed while the program waited calls nothing of the program's again.
        if (!out)
            state = State::failed;
        else if (state.load() != State::failed)
            for (auto* callback : callbacks)
                callback->stopped();
        drainNotes();
        prune();
    }

    bool waitOut()
    {
        const auto until = SteadyClock::now() + options.callTimeout;
        while (inCycle.load())
        {
            if (SteadyClock::now() > until)
                return false;
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        return true;
    }

    // Waits for the audio thread to have moved on to a list, or to be out of its cycle.
    bool waitSeen(std::uint64_t wanted)
    {
        const auto until = SteadyClock::now() + options.callTimeout;
        while (inCycle.load() && seen.load() < wanted)
        {
            if (SteadyClock::now() > until)
                return false;
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
        prune();
        return true;
    }

    void publish()
    {
        auto list = std::make_unique<Published>();
        list->generation = ++generation;
        list->callbacks = callbacks;
        published.store(list.get());
        lists.push_back(std::move(list));
        prune();
    }

    // Frees the lists the audio thread can no longer be reading: all but the newest while it is kept out, and
    // otherwise those older than the one it last took.
    void prune()
    {
        if (lists.size() <= 1)
            return;
        const bool out = !accepting.load() && !inCycle.load();
        const auto past = seen.load();
        std::erase_if(lists, [&](const std::unique_ptr<Published>& list) {
            return list.get() != published.load() && (out || list->generation < past);
        });
    }

    void lose(AudioLoss why, AudioError error = {})
    {
        resting = false;
        teardown();
        setState(State::lost);
        if (!error)
            error = {why == AudioLoss::unplugged ? "The audio device was unplugged."
                     : why == AudioLoss::takenExclusively ? "Another program took the audio device for its own use."
                     : why == AudioLoss::serviceRestarted ? "The computer's audio service restarted."
                     : why == AudioLoss::refused ? "The system refused the audio device."
                     : why == AudioLoss::clockLost ? "The audio device lost its clock."
                                                 : "The audio device stopped.",
                     "lost", lossCode.load()};
        tell({.kind = AudioEvent::Kind::lost, .loss = why, .error = error});
    }

    void handleSystem()
    {
        if (const auto why = lossPending.exchange(-1); why >= 0 && stream != nullptr)
        {
            if (follows() && static_cast<AudioLoss>(why) == AudioLoss::unplugged)
                orphan();
            else
                lose(static_cast<AudioLoss>(why));
        }

        if (restartPending.exchange(false) && stream != nullptr)
            lose(AudioLoss::serviceRestarted);

        if (mutePending.exchange(false) && stream != nullptr && run.inputs > 0)
            sayMute();

        if (devicesPending.exchange(false) && (stream != nullptr || resting))
        {
            const auto ids = system->deviceIds();
            if (std::find(ids.begin(), ids.end(), currentDevice()) == ids.end())
            {
                if (follows())
                    orphan();
                else
                    lose(AudioLoss::unplugged);
            }
        }

        if (defaultsPending.load() && steadyNanoseconds() - defaultsAt.load() >= nanoseconds(options.settle))
        {
            defaultsPending = false;
            followDefault();
        }

        if (sleepPending.exchange(false) && stream != nullptr)
        {
            teardown();
            setState(State::sleeping);
            sleptAt = system->now();
            wokeAt = -1;
            tell({.kind = AudioEvent::Kind::sleeping});
        }

        if (wakePending.exchange(false) && state.load() == State::sleeping)
            wokeAt = system->now();

        if (state.load() == State::sleeping && wokeAt >= 0 && system->now() - wokeAt >= nanoseconds(options.wakeDelay))
        {
            const auto slept = wokeAt - sleptAt;
            wokeAt = -1;
            if (auto error = openAndBegin(false))
                lose(AudioLoss::notAnswering, error);
            else
                tell({.kind = AudioEvent::Kind::woke, .nanoseconds = slept});
        }

        if (changePending.load() && steadyNanoseconds() - changeAt.load() >= nanoseconds(options.settle))
        {
            changePending = false;
            const auto asked = reopenPending.exchange(false);
            if (stream != nullptr)
            {
                const auto now = stream->info();
                AudioStreamInfo before;
                {
                    const std::lock_guard hold(lock);
                    before = info;
                }
                if (asked || !sameShape(now, before))
                    rebuild(before, now);
            }
        }
    }

    // Opens the device again as it now runs, after it changed under the stream; what another program set is kept.
    void rebuild(const AudioStreamInfo& before, const AudioStreamInfo& now)
    {
        teardown();

        // What the device let go of asked before it went is answered by this, and what the one opened now asks while
        // it opens, which a driver resetting at every open would ask for ever.
        reopenPending = false;
        rebuilding = true;
        struct Done
        {
            std::atomic<bool>& flag;
            ~Done() { flag = false; }
        } done{rebuilding};
        {
            const std::lock_guard hold(lock);
            request.sampleRate = now.sampleRate;
            request.blockSize = now.maxBlock;
        }
        if (auto error = openAndBegin(false))
        {
            lose(AudioLoss::other, error);
            return;
        }
        AudioStreamInfo after;
        {
            const std::lock_guard hold(lock);
            after = info;
        }
        tell({.kind = AudioEvent::Kind::formatChanged, .before = before, .after = after});
    }

    // Following the default output, as a stream with no input asks.
    bool follows()
    {
        const std::lock_guard hold(lock);
        return request.followDefault && request.inputChannels.empty();
    }

    std::string currentDevice()
    {
        const std::lock_guard hold(lock);
        return deviceId;
    }

    // Its device gone: closed, and moved to the default once the system has said which, or lost when there is none.
    void orphan()
    {
        teardown();
        resting = false;
        orphaned = true;
        defaultsAt = steadyNanoseconds();
        defaultsPending = true;
    }

    void followDefault()
    {
        const auto now = state.load();
        if (now != State::running && !orphaned)
            return;
        const auto next = system->defaultDevice(AudioDirection::output);
        const auto here = currentDevice();
        if (next.empty() || (next == here && !orphaned))
        {
            if (orphaned)
            {
                orphaned = false;
                lose(AudioLoss::unplugged);
            }
            return;
        }
        if (next != here && moving && !moving(next))
        {
            if (orphaned)
            {
                orphaned = false;
                lose(AudioLoss::unplugged, {"The new default output is open already.", "move"});
            }
            return;
        }
        teardown();
        orphaned = false;
        {
            const std::lock_guard hold(lock);
            deviceId = next;
        }
        if (auto error = openAndBegin(false))
        {
            lose(AudioLoss::other, error);
            return;
        }
        tell({.kind = AudioEvent::Kind::moved});
    }

    // An output whose callbacks have all been idle long enough stops its device, and starts it again, a new stretch,
    // as soon as one isn't.
    void watchIdle()
    {
        std::chrono::milliseconds after{};
        {
            const std::lock_guard hold(lock);
            after = request.idleAfter;
        }
        if (after.count() <= 0 || state.load() != State::running)
            return;
        const bool everyIdle = !callbacks.empty() &&
                               std::all_of(callbacks.begin(), callbacks.end(), [](const AudioCallback* c) { return c->idle(); });
        if (resting)
        {
            if (!everyIdle)
            {
                if (auto error = openAndBegin(false))
                    lose(AudioLoss::other, error);
                else
                    tell({.kind = AudioEvent::Kind::active});
            }
            return;
        }
        if (stream == nullptr || run.inputs > 0 || !everyIdle)
        {
            idleSince = -1;
            return;
        }
        const auto now = steadyNanoseconds();
        if (idleSince < 0)
            idleSince = now;
        else if (now - idleSince >= nanoseconds(after))
        {
            teardown();
            resting = true;
            idleSince = -1;
            tell({.kind = AudioEvent::Kind::idle});
        }
    }

    void watch()
    {
        if (stream == nullptr || state.load() != State::running)
            return;
        const auto now = system->now();
        const auto rate = run.rate > 0.0 ? run.rate : 48000.0;
        const auto block = 1e9 * run.maxBlock / rate;

        if (stretchCycles.load() == 0)
        {
            if (now - startedAt > nanoseconds(options.firstBlockTimeout))
            {
                teardown();
                setState(State::stopped);
                tell({.kind = AudioEvent::Kind::noSound,
                      .error = {"No sound came from the audio device. Check its clock source and its connection.",
                                "start"}});
            }
            return;
        }

        // What the system gave the audio thread is known once it has run.
        if (!priorityRead)
        {
            priorityRead = true;
            const auto given = stream->info().priority;
            const std::lock_guard hold(lock);
            info.priority = given;
        }

        const auto last = lastCycleEnds.load();
        const auto position = run.inputs > 0 ? inputPosition.load() : outputPosition.load();
        if (!stalled && static_cast<double>(now - last) > options.stallBlocks * block)
        {
            stalled = true;
            stalledAt = last;
            tell({.kind = AudioEvent::Kind::stalled, .position = position});
        }
        else if (stalled && last > stalledAt)
        {
            stalled = false;
            tell({.kind = AudioEvent::Kind::resumed, .position = position});
        }

        watchLate();

        const auto frames = framesThisStretch.load();
        const auto elapsed = now - windowAt;
        if (elapsed >= 500'000'000)
        {
            const auto expected = rate * static_cast<double>(elapsed) / 1e9;
            if (!runawaySaid && static_cast<double>(frames - windowFrames) > 1.5 * expected + 4.0 * run.maxBlock)
            {
                runawaySaid = true;
                tell({.kind = AudioEvent::Kind::runaway, .position = position});
            }
            if (elapsed > 10'000'000'000)
            {
                windowAt = now;
                windowFrames = frames;
                runawaySaid = false;
            }
        }
    }

    // Late blocks told: the first after a second of none at once, and those within a second of the last told together,
    // the worst of them described.
    void watchLate()
    {
        const auto count = overruns.load();
        const auto told = lateTold.load();
        const auto now = steadyNanoseconds();

        if (count <= told || (lateToldAt >= 0 && now - lateToldAt < 1'000'000'000))
            return;

        AudioEvent event{.kind = AudioEvent::Kind::late, .count = static_cast<int>(count - told)};

        // The worst, read whole: tried again while the audio thread writes it, and left out if one counted since took
        // its place.
        for (int attempt = 0; attempt < 100; ++attempt)
        {
            const auto sequence = late.sequence.load(std::memory_order_acquire);
            const auto index = late.index.load(std::memory_order_relaxed);
            const auto began = late.began.load(std::memory_order_relaxed);
            const auto took = late.took.load(std::memory_order_relaxed);
            const auto frames = late.frames.load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);

            if (sequence % 2 != 0 || late.sequence.load(std::memory_order_relaxed) != sequence)
                continue;

            if (index > told && index <= count)
            {
                event.nanoseconds = took;
                event.frames = frames;

                if (options.describeLate)
                    event.cause = options.describeLate(began, began + took);
            }

            break;
        }

        lateTold.store(count);
        lateToldAt = now;
        tell(std::move(event));
    }

    void drainNotes()
    {
        Note note;
        while (notes.pop(note))
        {
            AudioEvent event;
            event.direction = note.direction;
            event.position = note.position;
            event.frames = note.frames;
            event.reported = note.reported;
            event.kind = note.kind == Note::Kind::dropout       ? AudioEvent::Kind::dropout
                         : note.kind == Note::Kind::silentInput ? AudioEvent::Kind::silentInput
                                                                : AudioEvent::Kind::inputSound;
            tell(std::move(event));
        }
        if (const auto lost = notesLost.exchange(0); lost > 0)
            tell({.kind = AudioEvent::Kind::dropout, .frames = -1, .reported = false, .count = static_cast<int>(lost)});
    }

    void tell(AudioEvent event)
    {
        if (!options.onEvent || state.load() == State::failed)
            return;
        event.system = system->name();
        event.deviceId = deviceId;
        {
            const std::lock_guard hold(lock);
            event.deviceName = deviceName;
        }
        auto deliver = [handler = options.onEvent, event = std::move(event)] { handler(event); };
        if (options.post)
            options.post(std::move(deliver));
        else
            queue->post(std::move(deliver));
    }

    void nudge()
    {
        {
            const std::lock_guard hold(lock);
        }
        wake.notify_one();
    }

    // The system's calls

    void systemChanged(AudioSystemChange change) noexcept override
    {
        switch (change)
        {
            case AudioSystemChange::devices: devicesPending = true; break;
            case AudioSystemChange::defaults:
                if (!follows())
                    return;
                defaultsAt = steadyNanoseconds();
                defaultsPending = true;
                break;
            case AudioSystemChange::serviceRestarted: restartPending = true; break;
            case AudioSystemChange::sleeping: sleepPending = true; break;
            case AudioSystemChange::woke: wakePending = true; break;
            case AudioSystemChange::inputMute: mutePending = true; break;
        }
        nudge();
    }

    void changed() noexcept override
    {
        changeAt = steadyNanoseconds();
        changePending = true;
        if (!audioThread::isCurrent())
            nudge();
    }

    void reopen() noexcept override
    {
        if (rebuilding.load())
            return;
        reopenPending = true;
        changed();
    }

    void lost(AudioLoss why, std::int64_t code) noexcept override
    {
        lossCode = code;
        lossPending = static_cast<int>(why);
        if (!audioThread::isCurrent())
            nudge();
    }

    // The audio thread

    void cycle(const AudioSystemCycle& given) noexcept override
    {
        inCycle.store(true);
        if (!accepting.load())
        {
            inCycle.store(false);
            silence(given);
            return;
        }

        {
            const audioThread::detail::Marker marker;
            const NoDenormals noDenormals;
            const auto* list = published.load();
            if (list != nullptr)
                seen.store(list->generation);

            const auto began = SteadyClock::now();
            process(given, list);
            const auto took = std::chrono::duration<double>(SteadyClock::now() - began).count();

            if (given.frames > 0 && run.rate > 0.0)
            {
                const auto lasts = static_cast<double>(given.frames) / run.rate;
                if (took > lasts)
                    noteLate(overruns.fetch_add(1, std::memory_order_relaxed) + 1, began, took,
                             static_cast<int>(given.frames), took / lasts);
                load.store(load.load(std::memory_order_relaxed) * 0.95 + 0.05 * took / lasts,
                           std::memory_order_relaxed);
                lastCycleEnds.store(system->now() + static_cast<std::int64_t>(std::llround(1e9 * lasts)));
            }
            cycles.fetch_add(1, std::memory_order_relaxed);
            stretchCycles.fetch_add(1, std::memory_order_relaxed);
        }
        inCycle.store(false);
    }

    // Kept if it is the first late block since the last told, or worse than the worst since.
    void noteLate(std::int64_t index, SteadyClock::time_point began, double took, int frames, double share) noexcept
    {
        if (late.index.load(std::memory_order_relaxed) > lateTold.load(std::memory_order_relaxed) &&
            share <= late.share.load(std::memory_order_relaxed))
            return;

        const auto sequence = late.sequence.load(std::memory_order_relaxed);
        late.sequence.store(sequence + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        late.index.store(index, std::memory_order_relaxed);
        late.began.store(std::chrono::duration_cast<std::chrono::nanoseconds>(began.time_since_epoch()).count(),
                         std::memory_order_relaxed);
        late.took.store(std::llround(took * 1e9), std::memory_order_relaxed);
        late.frames.store(frames, std::memory_order_relaxed);
        late.share.store(share, std::memory_order_relaxed);
        late.sequence.store(sequence + 2, std::memory_order_release);
    }

    static void silence(const AudioSystemCycle& given) noexcept
    {
        if (given.outputs == nullptr)
            return;
        for (int c = 0; c < given.outputChannels; ++c)
            if (given.outputs[c] != nullptr)
                std::fill(given.outputs[c], given.outputs[c] + given.frames, 0.0f);
    }

    void noteDropout(AudioDirection direction, std::int64_t position, std::int64_t frames) noexcept
    {
        dropouts.fetch_add(1, std::memory_order_relaxed);
        if (frames > 0)
            lostFrames.fetch_add(frames, std::memory_order_relaxed);
        if (!notes.push({Note::Kind::dropout, direction, true, position, frames}))
            notesLost.fetch_add(1, std::memory_order_relaxed);
    }

    void note(Note::Kind kind, std::int64_t position) noexcept
    {
        if (!notes.push({kind, AudioDirection::input, true, position, -1}))
            notesLost.fetch_add(1, std::memory_order_relaxed);
    }

    void process(const AudioSystemCycle& given, const Published* list) noexcept
    {
        auto& r = run;
        silence(given);
        if (given.frames == 0)
            return;
        const auto frames = static_cast<std::int64_t>(given.frames);

        // The timeline: a jump in the system's positions is a gap of that many frames, and a gap flagged where the
        // system gives no positions is one of unknown length, except on the first cycle, which follows nothing.
        if (r.inputs > 0)
        {
            if (given.inputPosition >= 0)
            {
                if (r.expectInput >= 0 && given.inputPosition > r.expectInput)
                {
                    const auto jump = given.inputPosition - r.expectInput;
                    noteDropout(AudioDirection::input, r.inputPosition, jump);
                    r.inputPosition += jump;
                }
                r.expectInput = given.inputPosition + frames;
            }
            else if (given.inputGap && r.cycles > 0)
                noteDropout(AudioDirection::input, r.inputPosition, -1);
        }
        if (r.outputs > 0)
        {
            if (given.outputPosition >= 0)
            {
                if (r.expectOutput >= 0 && given.outputPosition > r.expectOutput)
                {
                    const auto jump = given.outputPosition - r.expectOutput;
                    noteDropout(AudioDirection::output, r.outputPosition, jump);
                    r.outputPosition += jump;
                }
                r.expectOutput = given.outputPosition + frames;
            }
            else if (given.outputGap && r.cycles > 0)
                noteDropout(AudioDirection::output, r.outputPosition, -1);
        }
        ++r.cycles;

        // The inputs, silence for any the system didn't give or said to take as silence.
        const float* zero = r.zeros.data();
        for (int c = 0; c < r.inputs; ++c)
        {
            const bool given_ = !given.inputSilent && given.inputs != nullptr && c < given.inputChannels &&
                                given.inputs[c] != nullptr;
            r.inputPointers[static_cast<std::size_t>(c)] = given_ ? given.inputs[c] : zero;
        }
        watchSilence(frames);

        // Blocks no larger than the stream announced.
        for (std::int64_t offset = 0; offset < frames;)
        {
            const auto n = std::min<std::int64_t>(r.maxBlock, frames - offset);
            for (int c = 0; c < r.inputs; ++c)
            {
                const auto* from = r.inputPointers[static_cast<std::size_t>(c)];
                r.chunkInputs[static_cast<std::size_t>(c)] = from == zero ? zero : from + offset;
            }
            for (int c = 0; c < r.outputs; ++c)
                r.chunkOutputs[static_cast<std::size_t>(c)] =
                    given.outputs != nullptr && c < given.outputChannels && given.outputs[c] != nullptr
                        ? given.outputs[c] + offset
                        : r.discard.data();

            AudioBlock block;
            block.inputs = r.chunkInputs.data();
            block.inputChannels = r.inputs;
            block.outputs = r.chunkOutputs.data();
            block.outputChannels = r.outputs;
            block.frames = static_cast<int>(n);
            block.inputPosition = r.inputPosition;
            block.outputPosition = r.outputPosition;
            block.inputTime = given.inputTime >= 0 ? given.inputTime + offsetTime(offset) : -1;
            block.outputTime = given.outputTime >= 0 ? given.outputTime + offsetTime(offset) : -1;
            block.sampleRate = r.rate;

            if (list != nullptr)
            {
                bool first = true;
                for (auto* callback : list->callbacks)
                {
                    if (first)
                    {
                        callback->process(block);
                        first = false;
                        continue;
                    }
                    // Each callback after the first writes to silent buffers of its own, added to the output.
                    for (auto& channel : r.scratch)
                        std::fill(channel.begin(), channel.begin() + n, 0.0f);
                    AudioBlock own = block;
                    own.outputs = r.scratchPointers.data();
                    callback->process(own);
                    for (int c = 0; c < r.outputs; ++c)
                    {
                        auto* to = r.chunkOutputs[static_cast<std::size_t>(c)];
                        const auto* from = r.scratch[static_cast<std::size_t>(c)].data();
                        for (std::int64_t i = 0; i < n; ++i)
                            to[i] += from[i];
                    }
                }
            }

            offset += n;
            r.inputPosition += n;
            r.outputPosition += n;
        }

        inputPosition.store(r.inputPosition, std::memory_order_relaxed);
        outputPosition.store(r.outputPosition, std::memory_order_relaxed);
        framesThisStretch.fetch_add(frames, std::memory_order_relaxed);
    }

    std::int64_t offsetTime(std::int64_t frames) const noexcept
    {
        return run.rate > 0.0 ? static_cast<std::int64_t>(std::llround(1e9 * static_cast<double>(frames) / run.rate))
                              : 0;
    }

    // An input giving nothing but exact zeros for silentLimit frames is said, and said again when sound returns.
    void watchSilence(std::int64_t frames) noexcept
    {
        auto& r = run;
        if (r.inputs == 0 || r.silentLimit <= 0)
            return;
        const float* zero = r.zeros.data();
        for (std::int64_t i = 0; i < frames; ++i)
        {
            bool silent = true;
            for (int c = 0; c < r.inputs && silent; ++c)
            {
                const auto* channel = r.inputPointers[static_cast<std::size_t>(c)];
                silent = channel == zero || channel[i] == 0.0f;
            }
            if (silent)
            {
                if (++r.silentFrames == r.silentLimit && !r.silentSaid)
                {
                    r.silentSaid = true;
                    note(Note::Kind::silentInput, r.inputPosition + i);
                }
            }
            else
            {
                if (r.silentSaid)
                {
                    r.silentSaid = false;
                    note(Note::Kind::inputSound, r.inputPosition + i);
                }
                r.silentFrames = 0;
            }
        }
    }

    // Ending

    void shutdown()
    {
        {
            const std::lock_guard hold(lock);
            quitting = true;
        }
        wake.notify_one();

        std::unique_lock hold(lock);
        const bool done = ended.wait_for(hold, options.callTimeout, [this] { return finished; });
        hold.unlock();
        if (done && control.joinable())
            control.join();
        else if (control.joinable())
            control.detach();   // the device holds it up; its memory stays with the thread

        if (closed)
            closed();
        if (queue != nullptr)
            queue->stop();
    }
};

AudioStream::AudioStream(std::shared_ptr<AudioSystem> system, std::string deviceId, AudioStreamRequest request)
    : AudioStream(std::move(system), std::move(deviceId), std::move(request), Options{})
{
}

AudioStream::AudioStream(std::shared_ptr<AudioSystem> system, std::string deviceId, AudioStreamRequest request,
                         Options options)
    : impl(std::make_shared<Impl>())
{
    impl->system = std::move(system);
    impl->deviceId = std::move(deviceId);
    impl->request = std::move(request);
    impl->options = std::move(options);
    if (!impl->options.post)
        impl->queue = devices::detail::EventQueue::make();
    impl->begin();
}

AudioStream::~AudioStream()
{
    impl->shutdown();
}

AudioError AudioStream::start()
{
    return impl->call({.kind = Command::Kind::start});
}

AudioError AudioStream::stop()
{
    return impl->call({.kind = Command::Kind::stop});
}

AudioError AudioStream::change(AudioStreamRequest request)
{
    return impl->call({.kind = Command::Kind::change, .request = std::move(request)});
}

AudioError AudioStream::showDeviceSettings()
{
    return impl->call({.kind = Command::Kind::settings});
}

AudioError AudioStream::add(AudioCallback& callback)
{
    return impl->call({.kind = Command::Kind::add, .callback = &callback});
}

AudioError AudioStream::remove(AudioCallback& callback)
{
    return impl->call({.kind = Command::Kind::remove, .callback = &callback});
}

AudioStream::State AudioStream::state() const noexcept
{
    return impl->state.load();
}

AudioStreamInfo AudioStream::info() const
{
    const std::lock_guard hold(impl->lock);
    return impl->info;
}

AudioStreamRequest AudioStream::request() const
{
    const std::lock_guard hold(impl->lock);
    return impl->request;
}

AudioStream::Statistics AudioStream::statistics() const noexcept
{
    Statistics now;
    now.cycles = impl->cycles.load();
    now.stretches = impl->stretches.load();
    now.inputPosition = impl->inputPosition.load();
    now.outputPosition = impl->outputPosition.load();
    now.dropouts = impl->dropouts.load();
    now.lostFrames = impl->lostFrames.load();
    now.overruns = impl->overruns.load();
    now.load = impl->load.load();
    return now;
}

AudioWorkgroup AudioStream::workgroup() const
{
    const std::lock_guard hold(impl->lock);
    return impl->workgroupNow;
}

std::string AudioStream::deviceId() const
{
    const std::lock_guard hold(impl->lock);
    return impl->deviceId;
}

AudioSystem& AudioStream::system() const noexcept
{
    return *impl->system;
}

void AudioStream::whenClosed(std::function<void()> closed)
{
    impl->closed = std::move(closed);
}

void AudioStream::whenMoving(std::function<bool(const std::string& to)> moving)
{
    impl->moving = std::move(moving);
}

}
