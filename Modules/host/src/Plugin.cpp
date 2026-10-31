#include "symbolengine/host/Plugin.h"

#include "CrashNoteRecords.h"
#include "Guard.h"
#include "MainThread.h"
#include "ProcessState.h"
#include "UnsafeCalls.h"


#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <semaphore>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace symbolengine
{

namespace
{

// Set while the calling thread is inside a plugin's process call, so a notice the plugin sends from there is only
// recorded, never posted, which would allocate on the audio thread.
thread_local bool insideProcess = false;

// Bits of a plugin's pending notices the host keeps for itself: its own edits waiting, and values from the audio thread
// waiting to be shown.
constexpr std::uint32_t notFiniteSeen = 1u << 27, unsafeCallSeen = 1u << 28, audioFailed = 1u << 29,
                        editsWaiting = 1u << 30, valuesWaiting = 1u << 31;

// Runs a call held by address, for guarded().
template <typename Call>
void run(void* call)
{
    (*static_cast<Call*>(call))();
}

std::int64_t nanosecondsNow() noexcept
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::int64_t ticksNow() noexcept
{
    return std::chrono::steady_clock::now().time_since_epoch().count();
}

std::int64_t ticksIn(std::chrono::milliseconds wait)
{
    return ticksNow() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(wait).count();
}

}

struct PluginHost::Shared : std::enable_shared_from_this<PluginHost::Shared>
{
    Options options;
    PluginCrashNote* note = nullptr;

    std::mutex lock;
    std::deque<std::pair<std::weak_ptr<Plugin::Inner>, PluginFormat*>> queue;
    bool making = false;

    // Plugins with notices sent from the audio thread, which a thread of the host's own hands to the main thread: it
    // looks every 10 ms, and is woken at once for a reset, which can't wait for its look.
    std::vector<std::weak_ptr<Plugin::Inner>> plugins;
    std::atomic<bool> audioNotices{false}, stopping{false};

    // What sees plugins' unsafe calls, for a host asked to watch for them where they can be.
    const unsafeCalls::Link* unsafeLink = nullptr;
    std::thread watcher;
    std::binary_semaphore wake{0};
    std::atomic<bool> woken{false};

    // From the audio thread: one signal, which never waits, until the watcher has woken.
    void wakeWatcher() noexcept
    {
        if (!woken.exchange(true))
            wake.release();
    }

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
    void sweepStuck();
    std::int64_t stuckLookedAt = 0;

    // The earliest a notice put off is due, on the steady clock, which the watcher looks out for; 0 for none.
    std::atomic<std::int64_t> noticesDue{0};

    void noticesDueAt(std::int64_t at) noexcept
    {
        for (auto earliest = noticesDue.load();
             (earliest == 0 || at < earliest) && !noticesDue.compare_exchange_weak(earliest, at);)
        {
        }
    }

    // Each plugin's description by its number, from when it is made until 64 more have gone after it.
    std::mutex namesLock;
    std::map<std::uint64_t, PluginDescription> names;
    std::deque<std::uint64_t> gone;
    std::uint64_t numbered = 0;

    std::uint64_t number(const PluginDescription& description)
    {
        const std::lock_guard held(namesLock);
        names.emplace(++numbered, description);
        return numbered;
    }

    void forget(std::uint64_t plugin)
    {
        const std::lock_guard held(namesLock);
        gone.push_back(plugin);

        if (gone.size() > 64)
        {
            names.erase(gone.front());
            gone.pop_front();
        }
    }

    // The last thousand blocks that took a quarter of their time or more, from any audio thread: which plugin, when it
    // began, how long it took and how long it lasts, each entry's sequence odd while it is written.
    struct Slow
    {
        std::atomic<std::uint32_t> sequence{0};
        std::atomic<std::uint64_t> plugin{0};
        std::atomic<std::int64_t> began{0}, took{0}, lasts{0};
    };

    std::array<Slow, 1024> slow{};
    std::atomic<std::uint64_t> slowWritten{0};

    void noteSlow(std::uint64_t plugin, std::int64_t began, std::int64_t took, std::int64_t lasts) noexcept
    {
        auto& entry = slow[slowWritten.fetch_add(1, std::memory_order_relaxed) % slow.size()];
        const auto sequence = entry.sequence.load(std::memory_order_relaxed);
        entry.sequence.store(sequence + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        entry.plugin.store(plugin, std::memory_order_relaxed);
        entry.began.store(began, std::memory_order_relaxed);
        entry.took.store(took, std::memory_order_relaxed);
        entry.lasts.store(lasts, std::memory_order_relaxed);
        entry.sequence.store(sequence + 2, std::memory_order_release);
    }
};

struct Plugin::Inner : PluginFormatClient, std::enable_shared_from_this<Plugin::Inner>
{
    std::shared_ptr<PluginHost::Shared> host;
    PluginDescription description;
    Plugin* owner = nullptr;

    // Its place in the program's crash note, which every call into it is recorded against.
    int noteSlot = -1;

    // Its number in the host, which names it in a late block a while after it is gone.
    std::uint64_t number = 0;

    // What it has been found calling on the audio thread, for a host that watches.
    std::unique_ptr<unsafeCalls::Record> unsafeRecord;

    ~Inner() override
    {
        crashNote::release(host->note, noteSlot);
        host->forget(number);
    }

    // The main thread's.
    std::unique_ptr<PluginFormatInstance> instance;
    PluginError problem;
    PluginLayout layout{};

    // The layout the program asked for, while the plugin takes it.
    std::optional<PluginLayout> askedLayout;
    PluginSetup setup{};
    int latency = 0, tail = 0;
    int busy = 0;
    bool activated = false;   // whether the plugin itself is active, which a failed one may still be
    std::vector<std::function<void()>> waiting;

    // Its parameters and their values as the host knows them.
    std::vector<PluginParameter> parameterList;
    std::map<std::uint32_t, double> values;

    // Whether it took a state while it wasn't prepared, which it may apply only once it is.
    bool readValuesOncePrepared = false;

    // The parameter being shown a value, an edit of which the plugin makes meanwhile being an echo of it, and until
    // when its own edits aren't the user's, after its state is given.
    std::atomic<std::int64_t> showing{-1}, quietUntil{0};

    // The plugin's own edits, from any thread, and whether each was the user's, until the main thread takes them; room
    // made as the plugin is made, so one from inside processing allocates nothing, and is passed over if the lock is
    // held just then.
    static constexpr std::size_t editRoom = 1024;
    std::mutex editLock;
    std::vector<std::pair<PluginEdit, bool>> edits = [] {
        std::vector<std::pair<PluginEdit, bool>> room;
        room.reserve(editRoom);
        return room;
    }();

    // The parameters as the audio thread knows them, each with the latest value for its processing to hear, and the
    // last value the program's automation gave it and the last its processing reported, kept for the main thread to
    // show. A new table is swapped in when they change, the old ones kept until the audio thread is next out of the
    // plugin.
    struct Slot
    {
        std::atomic<double> wanted{0.0}, automated{0.0}, reported{0.0};
        std::atomic<bool> wantedWaiting{false}, automatedWaiting{false}, reportedWaiting{false};
    };

    struct Table
    {
        std::vector<std::uint32_t> ids;
        std::unique_ptr<Slot[]> slots;

        Slot* find(std::uint32_t id) const noexcept
        {
            const auto found = std::lower_bound(ids.begin(), ids.end(), id);
            return found != ids.end() && *found == id ? &slots[static_cast<std::size_t>(found - ids.begin())] : nullptr;
        }
    };

    std::atomic<Table*> table{nullptr};
    std::vector<std::unique_ptr<Table>> tables;

    // Shared with the audio thread.
    std::atomic<State> state{State::making};
    std::atomic<bool> accepting{false}, inProcess{false}, processing{false};
    std::atomic<std::int64_t> blockBegan{0};   // when the block under way began, on the steady clock; 0 for none

    // Whether the main thread holds it off the audio for a call that can't overlap its processing, during which a block
    // is silent, never heard unprocessed.
    std::atomic<bool> holding{false};
    PluginFormatInstance* audioInstance = nullptr;
    std::vector<std::vector<float>> inputs, outputs;
    std::vector<const float*> inputPointers;
    std::vector<float*> outputPointers;

    // Whether any parameter has a value waiting for its processing to hear, so a block looks through them only then;
    // how many of those one block carries, the rest following with the next; and the count of blocks played, by which
    // the main thread knows a block has carried them.
    std::atomic<bool> valuesWanted{false};
    static constexpr std::size_t changesPerBlock = 1024;
    std::vector<PluginParameterChange> blockChanges =
        std::vector<PluginParameterChange>(static_cast<std::size_t>(maximumBlockChanges));
    std::vector<PluginParameterChange> reportedRoom = std::vector<PluginParameterChange>(256);
    std::atomic<std::uint64_t> blocksPlayed{0};
    std::int64_t framesPlayed = 0;

    // How long its processing takes, kept by the audio thread: the blocks it played and those it was late in; since its
    // timing was last taken, the nanoseconds it took and those its blocks lasted, and its worst block, its share as a
    // float above its nanoseconds, so a larger share is a larger number, with when it began.
    std::atomic<std::int64_t> blocksTimed{0}, lateBlocks{0}, took{0}, lasted{0}, worstAt{0};
    std::atomic<std::uint64_t> worst{0};

    // When it asked for a reset not yet done, and the longest it waited for one since its timing was last taken.
    std::atomic<std::int64_t> resetAsked{0}, longestResetWait{0};

    void timed(std::chrono::steady_clock::time_point began, int frames) noexcept
    {
        const auto taken =
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - began);
        const auto lasts = std::llround(frames * 1e9 / setup.rate);
        blocksTimed.fetch_add(1, std::memory_order_relaxed);
        took.fetch_add(taken.count(), std::memory_order_relaxed);
        lasted.fetch_add(lasts, std::memory_order_relaxed);

        if (taken.count() > lasts)
            lateBlocks.fetch_add(1, std::memory_order_relaxed);

        const auto at = std::chrono::duration_cast<std::chrono::nanoseconds>(began.time_since_epoch()).count();
        const auto share = static_cast<float>(static_cast<double>(taken.count()) / static_cast<double>(lasts));
        const auto packed = (std::uint64_t{std::bit_cast<std::uint32_t>(share)} << 32) |
                            static_cast<std::uint32_t>(std::min<std::int64_t>(taken.count(), 0xFFFFFFFF));

        for (auto current = worst.load(std::memory_order_relaxed); packed > current;)
            if (worst.compare_exchange_weak(current, packed, std::memory_order_relaxed))
            {
                worstAt.store(at, std::memory_order_relaxed);
                break;
            }

        if (share >= 0.25f)
            host->noteSlow(number, at, taken.count(), lasts);
    }

    PluginTiming timing(bool take) noexcept
    {
        PluginTiming measured;
        measured.blocks = blocksTimed.load(std::memory_order_relaxed);
        measured.lateBlocks = lateBlocks.load(std::memory_order_relaxed);
        const auto spent = take ? took.exchange(0) : took.load();
        const auto blocksLasted = take ? lasted.exchange(0) : lasted.load();
        const auto worstBlock = take ? worst.exchange(0) : worst.load();
        measured.longestResetWait = take ? longestResetWait.exchange(0) : longestResetWait.load();

        if (blocksLasted > 0)
            measured.load = static_cast<double>(spent) / static_cast<double>(blocksLasted);

        if (worstBlock != 0)
        {
            measured.worstShare = std::bit_cast<float>(static_cast<std::uint32_t>(worstBlock >> 32));
            measured.worstNanoseconds = static_cast<std::int64_t>(worstBlock & 0xFFFFFFFF);
            measured.worstAt = worstAt.load(std::memory_order_relaxed);
        }

        return measured;
    }

    // Whether it crashed, after which nothing calls it again, not even to take it down; and what failed on the audio
    // thread, with the fault if it crashed, for the main thread to say.
    std::atomic<bool> crashed{false};
    std::atomic<const char*> audioFailure{nullptr};
    std::atomic<const char*> audioReason{nullptr};
    std::atomic<std::uint32_t> audioFault{0};

    // Whether it has an editor, once asked, and whether one of its editors is open, in the frame given, the main
    // thread's.
    std::optional<bool> editorAnswer;
    bool editorOpen = false;
    PluginEditorFrame* editorFrame = nullptr;

    // Notices, recorded from any thread and acted on from the main thread. When a latency or bus notice may next
    // deactivate and activate it, after that changed nothing, and how many in a row did so.
    std::atomic<std::uint32_t> pending{0};
    std::atomic<bool> noticeScheduled{false};
    std::int64_t cycleAgainAt = 0;
    int fruitlessCycles = 0;

    // Blocks it spoiled with values that aren't numbers, which pass it by while it is reset, even while it is held; and
    // how often that has been done lately, since one still doing it isn't put right by another.
    std::atomic<std::int64_t> notFiniteCount{0};
    std::atomic<bool> notFinite{false}, fadingIn{false};
    std::int64_t lastSpoiledReset = 0;
    int spoiledResets = 0;
    static constexpr int mostSpoiledResets = 3;

    void notice(PluginNotice what) noexcept override
    {
        // The first asking for a reset, kept until it is done.
        if (has(what, PluginNotice::reset))
        {
            std::int64_t none = 0;
            resetAsked.compare_exchange_strong(none, nanosecondsNow());
        }

        pending.fetch_or(static_cast<std::uint32_t>(what));

        if (insideProcess)
        {
            host->audioNotices = true;

            if (has(what, PluginNotice::reset))
                host->wakeWatcher();

            return;
        }

        scheduleNotices();
    }

    void edit(const PluginEdit& edit) noexcept override
    {
        // Made while the host shows the plugin a value for the same parameter, an echo of it: not answered.
        if (!insideProcess && showing.load() == static_cast<std::int64_t>(edit.id) && host->onMainThread())
            return;

        const bool byUser = ticksNow() >= quietUntil.load();
        std::unique_lock held(editLock, std::defer_lock);

        if (insideProcess)
        {
            if (!held.try_lock())
                return;
        }
        else
        {
            held.lock();
        }

        if (edits.size() < editRoom)
            edits.emplace_back(edit, byUser);

        held.unlock();
        pending.fetch_or(editsWaiting);

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
    // changes of the whole process put back; and an exception from it, or on Windows a crash, failing the plugin
    // instead of reaching the program.
    template <typename Call>
    bool call(const char* step, Call&& body)
    {
        const crashNote::InCall noted(host->note, noteSlot, step, false);
        const bool outermost = busy++ == 0;
        const auto before = outermost ? ProcessState::now() : ProcessState{};
        bool fine = true;

        try
        {
            std::uint32_t fault = 0;

            if (!guarded(&run<std::remove_reference_t<Call>>, &body, fault))
            {
                crash(step, fault);
                fine = false;
            }
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

    void crash(const char* step, std::uint32_t fault)
    {
        crashed = true;
        fail(step, "It crashed, so it was switched off.");
        problem.code = fault;
        tell(PluginChange::crashed);

        if (editorFrame != nullptr)
            editorFrame->lost();
    }

    // A call its format makes of its own accord, as a message between its parts, made as the host's own are.
    void deliver(const char* step, const std::function<void()>& delivered) override
    {
        if (state == State::failed || crashed)
            return;

        call(step, [&] { delivered(); });
    }

    // A call into the plugin on the audio thread: an exception from it, or on Windows a crash, takes the plugin out of
    // the audio from the next block, and the main thread says so.
    template <typename Call>
    bool audioCall(const char* step, Call&& body) noexcept
    {
        const crashNote::InCall noted(host->note, noteSlot, step, true);
        // An offline render isn't watched: nothing is waiting for it, so a plugin may wait there.
        const unsafeCalls::Watching watching(setup.offline ? nullptr : unsafeRecord.get(), step,
                                             static_cast<std::int64_t>(blocksPlayed.load(std::memory_order_relaxed)));
        std::uint32_t fault = 0;

        try
        {
            if (guarded(&run<std::remove_reference_t<Call>>, &body, fault))
                return true;

            crashed = true;
        }
        catch (...)
        {
        }

        audioFail(step, fault, nullptr);
        return false;
    }

    // Takes the plugin out of the audio from the audio thread, for the main thread to say why.
    void audioFail(const char* step, std::uint32_t fault, const char* why) noexcept
    {
        accepting = false;
        audioFault = fault;
        audioReason = why;
        audioFailure = step;
        pending.fetch_or(audioFailed);
        host->audioNotices = true;
    }

    void fail(const char* step, std::string message)
    {
        accepting = false;
        holding = false;
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
        // Holding before accepting is left, so no block between the two passes unprocessed.
        holding = true;
        accepting = false;
        const auto until = std::chrono::steady_clock::now() + blockLimit();

        // The run loop turns meanwhile, as a plugin's processing may wait on this thread, and this marks it busy, so nothing
        // that comes of that turning overlaps with this.
        ++busy;
        while (inProcess)
        {
            if (std::chrono::steady_clock::now() > until)
            {
                --busy;
                leftOutAsStuck();
                return false;
            }

            if (host->options.turn)
                host->options.turn();
            else
                mainThread::turn();
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        --busy;

        // Tables the audio thread can no longer be reading.
        std::erase_if(tables, [this](const std::unique_ptr<Table>& old) { return old.get() != table.load(); });

        if (processing.exchange(false))
            return call("stopProcessing", [this] { instance->stopProcessing(); });

        return true;
    }

    // Ends a hold that leaves it off the audio, as when it is released, can't be prepared again or has failed: a block
    // passes as it came again.
    void endHold() { holding = false; }

    // How long one block may take: a realtime one the limit, and one being rendered as fast as it can be, however long a
    // plugin is allowed to take over it, as a slow one is how it is meant to be.
    std::chrono::milliseconds blockLimit() const
    {
        return setup.offline ? std::max(host->options.audioLimit, std::chrono::milliseconds(60'000))
                             : host->options.audioLimit;
    }

    void leftOutAsStuck()
    {
        if (state == State::failed)
            return;
        accepting = false;
        problem = {"It didn't return from processing audio within " + std::to_string(blockLimit().count()) + " ms.",
                   "process", 0};
        state = State::failed;
        endHold();
        tell(PluginChange::stuck);
    }

    // Takes values waiting for its processing to hear, at most those there is room for, the rest left for next time.
    std::size_t takeWanted(const Table& from, PluginParameterChange* into, std::size_t room) noexcept
    {
        std::size_t taken = 0;

        for (std::size_t i = 0; i < from.ids.size(); ++i)
        {
            auto& slot = from.slots[i];

            if (!slot.wantedWaiting.load())
                continue;

            if (taken == room)
            {
                valuesWanted = true;
                break;
            }

            slot.wantedWaiting = false;
            into[taken++] = {from.ids[i], 0, slot.wanted.load()};
        }

        return taken;
    }

    // Hands the plugin's processing what it hasn't heard, while it is active and nothing plays it.
    bool flushUnheard()
    {
        const auto* const now = table.load();

        if (now == nullptr || state != State::active)
            return true;

        std::vector<PluginParameterChange> changes;

        while (valuesWanted.exchange(false))
        {
            changes.resize(static_cast<std::size_t>(maximumBlockChanges));
            changes.resize(takeWanted(*now, changes.data(), changes.size()));

            if (!changes.empty() &&
                !call("flush", [&] { instance->flush(changes.data(), static_cast<int>(changes.size())); }))
                return false;
        }

        return true;
    }

    // Hands over what it hasn't heard while nothing has prepared it, for a format whose plugin hears values only in
    // processing, by preparing it for as long as that takes.
    bool flushUnprepared()
    {
        const bool fine = activate() && flushUnheard();

        if (activated)
            deactivate();

        if (state != State::failed)
            state = State::inactive;

        return fine;
    }

    // Lets go of every value waiting to be heard, with nothing playing it.
    void forgetWanted()
    {
        valuesWanted = false;

        if (const auto* const now = table.load())
            for (std::size_t i = 0; i < now->ids.size(); ++i)
                now->slots[i].wantedWaiting = false;
    }

    // The values it holds now.
    void readValues()
    {
        call("parameters", [this] {
            for (const auto& parameter : parameterList)
                values[parameter.id] = instance->parameterValue(parameter.id);
        });
    }

    void readParameters()
    {
        call("parameters", [this] {
            parameterList = instance->parameters();
            values.clear();
        });
        readValues();

        showAudioValues();
        auto made = std::make_unique<Table>();

        for (const auto& parameter : parameterList)
            made->ids.push_back(parameter.id);

        std::sort(made->ids.begin(), made->ids.end());
        made->ids.erase(std::unique(made->ids.begin(), made->ids.end()), made->ids.end());
        made->slots = std::make_unique<Slot[]>(made->ids.size());

        // Values not yet heard go with them.
        if (const auto* const old = table.load())
            for (std::size_t i = 0; i < old->ids.size(); ++i)
                if (auto* const slot = made->find(old->ids[i]);
                    old->slots[i].wantedWaiting.exchange(false) && slot != nullptr)
                {
                    slot->wanted = old->slots[i].wanted.load();
                    slot->wantedWaiting = true;
                    valuesWanted = true;
                }

        table = made.get();
        tables.push_back(std::move(made));
    }

    const PluginParameter* known(std::uint32_t id) const
    {
        const auto found = std::find_if(parameterList.begin(), parameterList.end(),
                                        [id](const PluginParameter& parameter) { return parameter.id == id; });
        return found != parameterList.end() ? &*found : nullptr;
    }

    // Shows the plugin a value, an edit it makes of the same parameter meanwhile being an echo.
    bool show(std::uint32_t id, double value)
    {
        showing = static_cast<std::int64_t>(id);
        const bool fine = call("setParameter", [&] { instance->showParameterValue(id, value); });
        showing = -1;
        return fine;
    }

    // A value for its processing to hear, by its next block or, while nothing plays it, by a flush. Only the latest of
    // each parameter is kept, so however many are set none is left out, and none comes back after a later one.
    void giveToAudio(std::uint32_t id, double value)
    {
        const auto* const now = table.load();

        if (auto* const slot = now != nullptr ? now->find(id) : nullptr)
        {
            slot->wanted = value;
            slot->wantedWaiting = true;
            valuesWanted = true;
        }
    }

    // Automation given while nothing plays it, from the audio thread: heard once something does, and shown.
    void keepAutomation(const PluginParameterChange* changes, int count) noexcept
    {
        const auto* const known = table.load();
        bool kept = false;

        for (int i = 0; known != nullptr && i < count; ++i)
            if (auto* const slot = known->find(changes[i].id))
            {
                slot->wanted = changes[i].value;
                slot->wantedWaiting = true;
                slot->automated = changes[i].value;
                slot->automatedWaiting = true;
                kept = true;
            }

        if (kept)
        {
            valuesWanted = true;
            pending.fetch_or(valuesWaiting);
            host->audioNotices = true;
        }
    }

    // A block it played holds a value that isn't a number, from the audio thread: that block and those after it pass
    // it by until the main thread has reset it.
    void spoiled() noexcept
    {
        notFinite = true;
        accepting = false;
        notFiniteCount.fetch_add(1, std::memory_order_relaxed);
        pending.fetch_or(notFiniteSeen);
        host->audioNotices = true;
        host->wakeWatcher();
    }

    // Resets one that spoiled a block, the audio going on as it came throughout, unless it does so again and again.
    void answerSpoiled()
    {
        const auto now = nanosecondsNow();
        constexpr std::int64_t soon = 1'000'000'000;
        spoiledResets = now - lastSpoiledReset < soon ? spoiledResets + 1 : 1;
        lastSpoiledReset = now;

        if (spoiledResets > mostSpoiledResets)
        {
            notFinite = false;
            fail("process", "It went on giving values that aren't numbers after being reset.");
            return;
        }

        tell(PluginChange::notFinite);
        notFinite = true;

        if (!hold() || !call("reset", [this] { instance->reset(); }))
            return;

        fadingIn = true;
        open();

        // After open, so the audio thread sees no hold with this cleared, which would be silent.
        notFinite = false;
    }

    void tellEdit(const PluginEdit& edit, bool byUser)
    {
        host->post([weak = weak_from_this(), edit, byUser] {
            if (const auto inner = weak.lock();
                inner != nullptr && inner->owner != nullptr && inner->owner->onEdit != nullptr)
                inner->owner->onEdit(edit, byUser);
        });
    }

    // Values the audio thread kept: the program's automation, shown to the plugin, and values the plugin's processing
    // reported, shown to it and told to the program.
    void showAudioValues()
    {
        const auto* const now = table.load();

        if (now == nullptr)
            return;

        for (std::size_t i = 0; i < now->ids.size() && state != State::failed; ++i)
        {
            auto& slot = now->slots[i];
            const auto id = now->ids[i];

            if (slot.automatedWaiting.exchange(false))
            {
                values[id] = slot.automated.load();
                show(id, values[id]);
            }

            if (slot.reportedWaiting.exchange(false))
            {
                values[id] = slot.reported.load();
                show(id, values[id]);
                tellEdit({PluginEdit::Kind::reported, id, values[id]}, false);
            }
        }
    }

    // The plugin's own edits: each value given to its processing, and shown to it if it doesn't show it already, and
    // every one told to the program; edits of parameters it never listed passed over.
    void takeEdits()
    {
        std::vector<std::pair<PluginEdit, bool>> taken;
        {
            const std::lock_guard held(editLock);
            taken.assign(edits.begin(), edits.end());
            edits.clear();
        }

        for (const auto& [edit, byUser] : taken)
        {
            if (state == State::failed)
                return;

            if (known(edit.id) == nullptr)
                continue;

            if (edit.kind == PluginEdit::Kind::value)
            {
                values[edit.id] = edit.value;
                giveToAudio(edit.id, edit.value);
                double shown = edit.value;

                if (!call("parameterValue", [&] { shown = instance->parameterValue(edit.id); }))
                    return;

                if (shown != edit.value && !show(edit.id, edit.value))
                    return;
            }

            tellEdit(edit, byUser);
        }
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
        holding = false;
    }

    bool activate()
    {
        PluginError error;
        bool done = false;

        if (!call("prepare", [&] { done = instance->activate(setup, error); }))
            return false;

        // A plugin may say its latency changed from inside these calls, which the reading below answers. Answering it
        // with another activation would bring the same notice again, for ever.
        pending.fetch_and(~static_cast<std::uint32_t>(PluginNotice::latency));
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

        framesPlayed = 0;
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

    // What the program asked for where it takes it, else stereo, else mono in and stereo out, else mono, else what it
    // settles on with audio out.
    bool settleLayout()
    {
        PluginLayout settled{};
        std::vector<PluginLayout> layouts{{2, 2}, {1, 2}, {1, 1}};

        if (askedLayout.has_value())
            layouts.insert(layouts.begin(), *askedLayout);

        for (const auto& wanted : layouts)
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

        auto raw = pending.exchange(0);
        constexpr auto cycling =
            static_cast<std::uint32_t>(PluginNotice::latency) | static_cast<std::uint32_t>(PluginNotice::buses);

        // Waited out after one that changed nothing, as a plugin can say so from what the activation itself brings on.
        if ((raw & cycling) != 0 && nanosecondsNow() < cycleAgainAt)
        {
            pending.fetch_or(raw & cycling);
            host->noticesDueAt(cycleAgainAt);
            raw &= ~cycling;
        }

        const auto what = static_cast<PluginNotice>(raw);

        if (const auto* const step = audioFailure.exchange(nullptr); step != nullptr && state != State::failed)
        {
            if (audioFault != 0)
                crash(step, audioFault);
            else if (const auto* const why = audioReason.load())
                fail(step, why);
            else
                fail(step, "It failed with its own error while processing audio.");
        }

        if ((raw & unsafeCallSeen) != 0)
            tell(PluginChange::unsafeCall);

        if ((raw & notFiniteSeen) != 0 && state == State::active)
            answerSpoiled();

        if ((raw & valuesWaiting) != 0)
            showAudioValues();

        if ((raw & editsWaiting) != 0)
            takeEdits();

        if (state == State::failed)
            return;

        // Reset with nothing playing it, as a reset is made.
        if (has(what, PluginNotice::reset))
        {
            const auto asked = resetAsked.exchange(0);

            if (state == State::active)
            {
                if (!hold() || !call("reset", [this] { instance->reset(); }))
                    return;

                // None asked, for a notice that came as the last was handled, which that reset answered.
                const auto wait = asked != 0 ? nanosecondsNow() - asked : 0;

                for (auto longest = longestResetWait.load();
                     wait > longest && !longestResetWait.compare_exchange_weak(longest, wait);)
                {
                }

                open();
            }
        }

        if (has(what, PluginNotice::buses) || has(what, PluginNotice::latency))
        {
            const bool wasActive = state == State::active;
            const auto before = std::make_pair(latency, tail);
            const auto layoutBefore = layout;

            if ((wasActive && (!hold() || !deactivate())) || state == State::failed)
            {
                endHold();
                return;
            }

            state = State::inactive;

            if (has(what, PluginNotice::buses) && !settleLayout())
            {
                endHold();
                tell(PluginChange::failed);
                return;
            }

            if (wasActive && !activate())
            {
                endHold();
                tell(PluginChange::failed);
                return;
            }

            open();

            if (has(what, PluginNotice::buses) && layout != layoutBefore)
                tell(PluginChange::buses);

            if (has(what, PluginNotice::latency) || std::make_pair(latency, tail) != before)
                tell(PluginChange::latency);

            if (layout == layoutBefore && std::make_pair(latency, tail) == before)
            {
                constexpr std::int64_t second = 1'000'000'000;
                constexpr int mostDoublings = 6;
                cycleAgainAt = nanosecondsNow() + (second << std::min(fruitlessCycles, mostDoublings));
                ++fruitlessCycles;
            }
            else
            {
                cycleAgainAt = 0;
                fruitlessCycles = 0;
            }
        }

        // Values changing needs only the values read, where the list itself changing needs everything made again.
        if (has(what, PluginNotice::parameters))
            readParameters();
        else if (has(what, PluginNotice::values))
            readValues();

        if (has(what, PluginNotice::parameters))
            tell(PluginChange::parameters);

        if (has(what, PluginNotice::values))
            tell(PluginChange::values);

        if (has(what, PluginNotice::state) && ticksNow() >= quietUntil.load())
            tell(PluginChange::state);

        if (has(what, PluginNotice::reload))
            tell(PluginChange::reload);

        if (has(what, PluginNotice::editor))
            tell(PluginChange::editor);
    }

    // Still inside a process call it never returned from, or crashed: its threads and timers may still run, and return
    // into what they were given or call back through it. So none of it is ever freed, and nothing calls it again.
    void keepForever()
    {
        static auto* const kept = new std::vector<std::shared_ptr<Inner>>();
        kept->push_back(shared_from_this());
    }

    // Taken down in order on the main thread: processing stopped, reset, deactivated, taken apart.
    void takeDown()
    {
        owner = nullptr;

        if (instance == nullptr)
            return;

        if (((state == State::failed && inProcess) || crashed) || !hold())
        {
            keepForever();
            return;
        }

        if (activated)
        {
            call("reset", [this] { instance->reset(); });
            deactivate();
        }

        call("destroy", [this] { instance.reset(); });
        state = State::failed;
        endHold();
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

void PluginHost::Shared::sweepStuck()
{
    std::vector<std::shared_ptr<Plugin::Inner>> live;
    {
        const std::lock_guard held(lock);
        for (const auto& plugin : plugins)
            if (auto inner = plugin.lock())
                live.push_back(std::move(inner));
    }

    const auto now = nanosecondsNow();
    for (const auto& plugin : live)
        if (const auto began = plugin->blockBegan.load(std::memory_order_relaxed);
            began != 0 && now - began > std::chrono::nanoseconds(plugin->blockLimit()).count())
            plugin->leftOutAsStuck();
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

namespace
{
bool watchingWorks();
}

PluginHost::PluginHost(Options options) : shared(std::make_shared<Shared>())
{
    shared->options = std::move(options);

    // Made on the main thread, it readies that thread for plugins before any is loaded.
    if (shared->options.onMainThread != nullptr && shared->options.onMainThread())
        mainThread::prepare();

    if (shared->options.watchUnsafeCalls && watchingWorks())
        shared->unsafeLink = unsafeCalls::link();

    // Notices a plugin sends from inside its process call are only recorded there; this thread passes them to the main
    // thread.
    shared->watcher = std::thread([weak = std::weak_ptr(shared)] {
        for (;;)
        {
            {
                const auto host = weak.lock();

                if (host == nullptr)
                    return;

                if (host->wake.try_acquire_for(std::chrono::milliseconds(10)))
                    host->woken = false;
            }

            const auto host = weak.lock();

            if (host == nullptr || host->stopping)
                return;

            if (const auto due = host->noticesDue.load(); due != 0 && nanosecondsNow() >= due)
            {
                host->noticesDue = 0;
                host->audioNotices = true;
            }

            if (host->audioNotices.exchange(false))
                host->post([host] { host->sweepNotices(); });

            // A plugin that won't come back from its process call, which nothing else would notice while the audio thread
            // is inside it. Looked at far less often than this wakes, and from the main thread, where it is safe to say.
            if (const auto now = nanosecondsNow(); now - host->stuckLookedAt > 200'000'000)
            {
                host->stuckLookedAt = now;
                host->post([host] { host->sweepStuck(); });
            }
        }
    });
}

PluginHost::~PluginHost()
{
    shared->stopping = true;
    shared->wakeWatcher();

    if (shared->watcher.joinable())
        shared->watcher.join();
}

namespace
{
bool watchingWorks()
{
    // Tried for real once, as being there isn't enough: a library loaded too late, or something else taking the same
    // calls first, sees nothing. The first look at a stack may load what it needs, which is done here too, not on an
    // audio thread.
    static const bool can = [] {
        const auto* const link = unsafeCalls::link();

        if (link == nullptr)
            return false;

        link->listen(&unsafeCalls::Record::saw);
        link->refresh();
        unsafeCalls::Record probe(*link, {}, nullptr, nullptr);
        probe.enter("probe", 0);
        unsafeCalls::tryCalls();
        probe.leave();
        const auto calls = probe.calls();
        const auto madeOne = [&](PluginUnsafeCall::Kind kind) {
            return std::any_of(calls.begin(), calls.end(), [&](const auto& call) { return call.kind == kind; });
        };
        return madeOne(PluginUnsafeCall::Kind::memory) && madeOne(PluginUnsafeCall::Kind::lock);
    }();

    return can;
}
}

bool PluginHost::canWatchUnsafeCalls()
{
#if defined(_WIN32)
    // Trying it on Windows changes the import table of every module loaded, with the program's threads running through
    // them, which asking whether it could be done mustn't do.
    return unsafeCalls::link() != nullptr;
#else
    return watchingWorks();
#endif
}

void PluginHost::setCrashNote(PluginCrashNote* note)
{
    shared->note = note;
}

std::optional<PluginLate> PluginHost::slowestBetween(std::int64_t from, std::int64_t to) const
{
    struct Block
    {
        std::uint64_t plugin = 0;
        std::int64_t began = 0, took = 0, lasts = 0;

        double share() const { return static_cast<double>(took) / static_cast<double>(lasts); }
    };

    std::optional<Block> alone, longest;

    for (const auto& entry : shared->slow)
    {
        const auto sequence = entry.sequence.load(std::memory_order_acquire);
        const Block block{entry.plugin.load(std::memory_order_relaxed), entry.began.load(std::memory_order_relaxed),
                          entry.took.load(std::memory_order_relaxed), entry.lasts.load(std::memory_order_relaxed)};
        std::atomic_thread_fence(std::memory_order_acquire);

        // Left out while it is written, never written, or outside the time.
        if (sequence % 2 != 0 || entry.sequence.load(std::memory_order_relaxed) != sequence || block.plugin == 0 ||
            block.began >= to || block.began + block.took <= from)
            continue;

        if (block.took > block.lasts)
        {
            if (!alone || block.share() > alone->share())
                alone = block;
        }
        else if (!longest || block.took > longest->took)
        {
            longest = block;
        }
    }

    const auto& chosen = alone ? alone : longest;

    if (!chosen)
        return std::nullopt;

    PluginLate late;
    late.alone = alone.has_value();
    late.nanoseconds = chosen->took;
    late.blockNanoseconds = chosen->lasts;
    late.at = chosen->began;
    const std::lock_guard held(shared->namesLock);

    if (const auto found = shared->names.find(chosen->plugin); found != shared->names.end())
        late.plugin = found->second;

    return late;
}

std::shared_ptr<Plugin> PluginHost::make(PluginFormat& format, const PluginDescription& description)
{
    auto inner = std::make_shared<Plugin::Inner>();
    inner->host = shared;
    inner->description = description;
    inner->noteSlot = crashNote::claim(shared->note, description);
    inner->number = shared->number(description);

    if (shared->unsafeLink != nullptr)
        inner->unsafeRecord = std::make_unique<unsafeCalls::Record>(
            *shared->unsafeLink, description.location,
            [](void* context) {
                auto& held = *static_cast<Plugin::Inner*>(context);
                held.pending.fetch_or(unsafeCallSeen);
                held.host->audioNotices = true;
            },
            inner.get());

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

bool Plugin::hasCrashed() const
{
    return inner->crashed;
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

PluginLayout Plugin::arrange(PluginLayout wanted)
{
    auto& held = *inner;

    if (!held.usable("arrange"))
        return held.layout;

    if (held.state != State::inactive)
    {
        held.host->misuse("arrange was called while the plugin was prepared");
        return held.layout;
    }

    held.askedLayout = wanted;

    if (!held.settleLayout() || held.layout != wanted)
        held.askedLayout.reset();

    return held.layout;
}

bool Plugin::prepare(const PluginSetup& setup)
{
    if (!inner->usable("prepare"))
        return false;

    // What the plugin has loaded by now is seen from here on, where that has to be asked for.
    if (const auto* const link = inner->host->unsafeLink)
        link->refresh();

    if ((inner->state == State::active && (!inner->hold() || !inner->deactivate())) || inner->state == State::failed)
    {
        inner->endHold();
        return false;
    }

    inner->state = State::inactive;
    inner->setup = setup;

    // Values set while it wasn't prepared reach its processing before any block.
    if (!inner->activate() || !inner->flushUnheard())
    {
        inner->endHold();
        return false;
    }

    if (std::exchange(inner->readValuesOncePrepared, false))
        inner->readValues();

    inner->open();
    return true;
}

void Plugin::release()
{
    if (!inner->usable("release") || inner->state != State::active)
        return;

    if (inner->hold() && inner->deactivate() && inner->state != State::failed)
        inner->state = State::inactive;

    inner->endHold();
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

std::int64_t Plugin::notFiniteBlocks() const
{
    return inner->notFiniteCount.load(std::memory_order_relaxed);
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

    if (held.accepting && held.valuesWanted)
    {
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
        const auto before = held.blocksPlayed.load();

        while (held.accepting && std::chrono::steady_clock::now() < until &&
               (held.valuesWanted || held.blocksPlayed.load() <= before))
            std::this_thread::sleep_for(std::chrono::microseconds(200));
    }

    if (held.valuesWanted && held.state == State::inactive)
    {
        if (!held.instance->takesValuesUnprepared() && !held.flushUnprepared() && held.state == State::failed)
            return std::nullopt;
    }
    else if (held.valuesWanted)
    {
        const bool wasAccepting = held.accepting;

        if (!held.hold() || !held.flushUnheard())
        {
            held.endHold();
            return std::nullopt;
        }

        if (wasAccepting)
            held.open();
        else
            held.endHold();
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

    // What the audio thread kept from before the state comes before it.
    inner->showAudioValues();

    PluginError error;
    bool loaded = false;

    // The plugin's edits and its state changing as it takes the state, and for a while after, aren't the user's.
    inner->quietUntil = ticksIn(std::chrono::milliseconds(250));

    if (!inner->call("loadState", [&] { loaded = inner->instance->setState(state, use, error); }))
        return false;

    inner->quietUntil = ticksIn(std::chrono::milliseconds(250));

    if (!loaded)
        inner->problem = error ? error : PluginError{"It didn't take the state.", "loadState", 0};
    else
    {
        // It says what the plugin holds now, so values set before it give way to it.
        inner->forgetWanted();
        inner->readValues();
        inner->readValuesOncePrepared = !active;
    }

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

    if (!inner->show(id, value))
        return false;

    inner->giveToAudio(id, value);
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

namespace
{
// Whether every sample of a block is a number and finite: checked for every block, so by its bits, which a compiler can
// do many at a time.
bool allFinite(const std::vector<std::vector<float>>& buffers, int frames) noexcept
{
    constexpr std::uint32_t exponent = 0x7F800000;
    std::uint32_t found = 0;

    for (const auto& buffer : buffers)
        for (int i = 0; i < frames; ++i)
            found |= static_cast<std::uint32_t>(
                (std::bit_cast<std::uint32_t>(buffer[static_cast<std::size_t>(i)]) & exponent) == exponent);

    return found == 0;
}
}

void Plugin::process(float* const* channels, int channelCount, int frames) noexcept
{
    process(channels, channelCount, frames, nullptr, 0);
}

void Plugin::process(float* const* channels, int channelCount, int frames, const PluginParameterChange* changes,
                     int changeCount, const PluginTime* time) noexcept
{
    auto& held = *inner;

    if (channelCount <= 0 || frames <= 0)
        return;

    // Entered before accepting is read, as the main thread leaves accepting before reading inProcess, so the two never
    // both go ahead.
    held.inProcess = true;

    if (!held.accepting)
    {
        held.keepAutomation(changes, changeCount);

        if (held.holding && !held.notFinite)
            for (int c = 0; c < channelCount; ++c)
                std::fill_n(channels[c], frames, 0.0f);

        held.inProcess = false;
        return;
    }

    insideProcess = true;
    const auto began = std::chrono::steady_clock::now();
    held.blockBegan.store(std::chrono::duration_cast<std::chrono::nanoseconds>(began.time_since_epoch()).count(),
                          std::memory_order_relaxed);
    auto* plugin = held.audioInstance;
    const auto modes = floatingPointModes();

    // A discontinuity clears what the plugin holds of the audio before it.
    bool going = true;

    if (!held.processing.exchange(true))
        going = held.audioCall("startProcessing", [plugin] { plugin->startProcessing(); });
    else if (time != nullptr && time->discontinuity)
        going = held.audioCall("process", [plugin] {
            plugin->stopProcessing();
            plugin->startProcessing();
        });

    const int ins = static_cast<int>(held.inputs.size());
    const int outs = static_cast<int>(held.outputs.size());
    const int most = std::max(1, held.setup.maximumFrames);
    const auto* const known = held.table.load();
    bool valuesKept = false;
    int next = 0;

    for (int offset = 0; going && offset < frames; offset += most)
    {
        const int count = std::min(most, frames - offset);

        for (int c = 0; c < ins; ++c)
            std::copy_n(channels[std::min(c, channelCount - 1)] + offset, count,
                        held.inputs[static_cast<std::size_t>(c)].begin());

        for (auto& output : held.outputs)
            std::fill_n(output.begin(), count, 0.0f);

        // The values the main thread set since the last block, at the start of this one, then the program's that fall
        // in it, at their frames within it, each kept to be shown to the plugin; any for a parameter it doesn't have
        // passed over.
        std::size_t given = 0;

        if (offset == 0 && known != nullptr && held.valuesWanted.exchange(false))
            given = held.takeWanted(*known, held.blockChanges.data(), Inner::changesPerBlock);

        for (; next < changeCount && std::clamp(changes[next].offset, 0, frames - 1) < offset + count; ++next)
        {
            auto* const slot = known != nullptr ? known->find(changes[next].id) : nullptr;

            if (slot == nullptr || given == held.blockChanges.size())
                continue;

            held.blockChanges[given++] = {changes[next].id, std::clamp(changes[next].offset - offset, 0, count - 1),
                                          changes[next].value};
            slot->automated.store(changes[next].value);
            slot->automatedWaiting.store(true);
            valuesKept = true;
        }

        // The program's time at this block's own first frame; without it, a position counting the frames played.
        PluginTime blockTime;

        if (time != nullptr)
        {
            blockTime = *time;
            blockTime.position += offset;
            blockTime.discontinuity = time->discontinuity && offset == 0;

            if (time->systemNanoseconds != 0)
                blockTime.systemNanoseconds += std::llround(offset * 1e9 / held.setup.rate);
        }
        else
        {
            blockTime.position = held.framesPlayed;
            blockTime.systemNanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                              std::chrono::steady_clock::now().time_since_epoch())
                                              .count();
        }

        PluginBlock block{held.inputPointers.data(),
                          held.outputPointers.data(),
                          ins,
                          outs,
                          count,
                          held.blockChanges.data(),
                          static_cast<int>(given),
                          held.reportedRoom.data(),
                          static_cast<int>(held.reportedRoom.size()),
                          0,
                          &blockTime};
        bool fine = false;
        going = held.audioCall("process", [&] { fine = plugin->process(block); });
        held.framesPlayed += count;

        // A plugin its format knows can never work again is taken out of the audio.
        if (going && !fine)
            if (const auto* const why = plugin->stopped())
            {
                held.audioFail("process", 0, why);
                going = false;
            }

        for (int r = 0; r < std::min(block.reportedCount, block.reportedRoom); ++r)
            if (auto* const slot = known != nullptr ? known->find(block.reported[r].id) : nullptr)
            {
                slot->reported.store(block.reported[r].value);
                slot->reportedWaiting.store(true);
                valuesKept = true;
            }

        // A plugin's changes to the thread's floating-point modes go no further than its own call.
        if (floatingPointModes() != modes)
            setFloatingPointModes(modes);

        // A block it failed is heard as it came.
        if (!fine || outs == 0)
            continue;

        // So is one holding a value that isn't a number, which would stay in whatever it reached from then on. One that
        // came in with the audio isn't held against it.
        if (!allFinite(held.outputs, count))
        {
            if (allFinite(held.inputs, count))
            {
                held.spoiled();
                going = false;
            }

            continue;
        }

        // Back from a reset, from the audio as it came to its own, over this block.
        const bool fading = held.fadingIn.exchange(false, std::memory_order_relaxed);

        for (int c = 0; c < channelCount; ++c)
        {
            const auto& from = held.outputs[static_cast<std::size_t>(std::min(c, outs - 1))];

            if (!fading)
            {
                std::copy_n(from.begin(), count, channels[c] + offset);
                continue;
            }

            for (int i = 0; i < count; ++i)
            {
                const auto share = static_cast<float>(i + 1) / static_cast<float>(count);
                auto& sample = channels[c][offset + i];
                sample += (from[static_cast<std::size_t>(i)] - sample) * share;
            }
        }
    }

    if (valuesKept)
    {
        held.pending.fetch_or(valuesWaiting);
        held.host->audioNotices = true;
    }

    held.timed(began, frames);
    ++held.blocksPlayed;
    insideProcess = false;
    held.blockBegan.store(0, std::memory_order_relaxed);
    held.inProcess = false;
}

PluginTiming Plugin::timing() const
{
    return inner->timing(false);
}

PluginTiming Plugin::takeTiming()
{
    return inner->timing(true);
}

std::vector<PluginUnsafeCall> Plugin::unsafeCalls() const
{
    return inner->unsafeRecord != nullptr ? inner->unsafeRecord->calls() : std::vector<PluginUnsafeCall>{};
}

std::int64_t Plugin::unsafeCallsNotListed() const
{
    return inner->unsafeRecord != nullptr ? inner->unsafeRecord->notListed() : 0;
}

bool Plugin::hasEditor()
{
    auto& held = *inner;

    if (held.editorAnswer.has_value())
        return *held.editorAnswer;

    if (!held.usable("hasEditor"))
        return false;

    bool has = false;

    if (!held.call("hasEditor", [&] { has = held.instance->hasEditor(); }))
        return false;

    held.editorAnswer = has;
    return has;
}

void Plugin::editAsEditor(const PluginEdit& edit)
{
    if (!inner->host->onMainThread())
    {
        inner->host->misuse("editAsEditor was called on a thread other than the main thread");
        return;
    }

    if (inner->state != State::failed)
        inner->edit(edit);
}

bool Plugin::editorOpen() const
{
    return inner->editorOpen;
}

std::unique_ptr<PluginEditor> PluginEditor::open(std::shared_ptr<Plugin> plugin, void* view, PluginEditorFrame& frame,
                                                 PluginError& error)
{
    error = {};

    if (plugin == nullptr)
    {
        error = {"There is no plugin.", "openEditor", 0};
        return nullptr;
    }

    auto& inner = *plugin->inner;

    if (!inner.usable("openEditor"))
    {
        error = inner.state == Plugin::State::failed ? inner.problem
                : inner.busy > 0                     ? PluginError{"A call into it is under way.", "openEditor", 0}
                                                     : PluginError{"It can't be called now.", "openEditor", 0};
        return nullptr;
    }

    if (inner.editorOpen)
    {
        error = {"Its editor is open already.", "openEditor", 0};
        return nullptr;
    }

    std::unique_ptr<PluginFormatEditor> made;
    PluginError failed;

    if (!inner.call("openEditor", [&] { made = inner.instance->openEditor(view, frame, failed); }))
    {
        error = inner.problem;
        return nullptr;
    }

    if (made == nullptr)
    {
        error = failed ? failed : PluginError{"Its editor couldn't be opened.", "openEditor", 0};
        return nullptr;
    }

    inner.editorOpen = true;
    inner.editorAnswer = true;
    inner.editorFrame = &frame;
    std::unique_ptr<PluginEditor> opened(new PluginEditor(std::move(plugin), std::move(made)));
    inner.call("resizable", [&] { opened->canResize = opened->editor->resizable(); });
    return opened;
}

PluginEditor::PluginEditor(std::shared_ptr<Plugin> plugin, std::unique_ptr<PluginFormatEditor> made)
    : held(std::move(plugin)), editor(std::move(made))
{
}

PluginEditor::~PluginEditor()
{
    const auto inner = held->inner;

    if (!inner->host->onMainThread())
        inner->host->misuse("an editor was closed on a thread other than the main thread");

    // The editor closed, or, from a plugin that crashed, never called again and kept; then the plugin let go of a turn
    // of the run loop later, so whatever the editor's closing started finishes first.
    auto kept = std::make_shared<std::unique_ptr<PluginFormatEditor>>(std::move(editor));
    auto close = [inner, kept, plugin = std::move(held)]() mutable {
        // Nor is a plugin stuck in its processing called, as closing its editor would very likely wait on the same lock.
        if (inner->crashed || (inner->state == Plugin::State::failed && inner->inProcess))
            static_cast<void>(kept->release());
        else
            inner->call("closeEditor", [&] { kept->reset(); });

        inner->editorOpen = false;
        inner->editorFrame = nullptr;
        inner->host->post([plugin = std::move(plugin)] {});
    };

    // During a call into the plugin, once it returns.
    if (inner->busy > 0)
        inner->waiting.push_back(std::move(close));
    else
        close();
}

PluginEditorSize PluginEditor::size() const
{
    return editor->size();
}

bool PluginEditor::resizable() const
{
    return canResize;
}

PluginEditorSize PluginEditor::constrain(PluginEditorSize wanted)
{
    auto& inner = *held->inner;

    if (!canResize || inner.busy > 0 || inner.state == Plugin::State::failed)
        return editor->size();

    auto taken = wanted;

    if (!inner.call("constrain", [&] { taken = editor->constrain(wanted); }))
        return editor->size();

    return taken;
}

void PluginEditor::resized(PluginEditorSize size)
{
    auto& inner = *held->inner;

    if (inner.state != Plugin::State::failed)
        inner.call("resized", [&] { editor->resized(size); });
}

void PluginEditor::rescaled(double scale)
{
    auto& inner = *held->inner;

    if (inner.state != Plugin::State::failed)
        inner.call("rescaled", [&] { editor->rescaled(scale); });
}

bool PluginEditor::keyDown(const PluginKey& key)
{
    auto& inner = *held->inner;
    bool taken = false;

    if (inner.state != Plugin::State::failed && inner.busy == 0)
        inner.call("keyDown", [&] { taken = editor->keyDown(key); });

    return taken;
}

bool PluginEditor::keyUp(const PluginKey& key)
{
    auto& inner = *held->inner;
    bool taken = false;

    if (inner.state != Plugin::State::failed && inner.busy == 0)
        inner.call("keyUp", [&] { taken = editor->keyUp(key); });

    return taken;
}

}
