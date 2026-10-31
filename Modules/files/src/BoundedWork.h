#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace symbolengine::detail
{

/** A thread for work in another's code that may never come back, as a system's decoder has been seen not to, so that whoever
    asks gets an answer all the same. What the work uses it must hold itself, as it may outlast this. */
class BoundedWork
{
public:
    BoundedWork() : state(std::make_shared<State>())
    {
        thread = std::thread([held = state] { loop(*held); });
    }

    ~BoundedWork()
    {
        {
            const std::lock_guard hold(state->lock);
            state->ending = true;
        }
        state->wake.notify_all();

        if (stuck())
            thread.detach();
        else
            thread.join();
    }

    BoundedWork(const BoundedWork&) = delete;
    BoundedWork& operator=(const BoundedWork&) = delete;

    /** Runs the work on this thread, and waits for it. False if it went for the limit without finishing or, when it is given
        one, moving the count on. From then on this thread is left to it, and no more work is taken. */
    bool run(std::function<void()> work, std::chrono::milliseconds limit,
             const std::atomic<std::uint64_t>* progress = nullptr)
    {
        std::unique_lock hold(state->lock);
        if (state->abandoned)
            return false;

        state->work = std::move(work);
        state->done = false;
        state->wake.notify_all();

        auto seen = progress != nullptr ? progress->load() : 0;
        auto until = std::chrono::steady_clock::now() + limit;
        while (!state->done)
        {
            state->finished.wait_for(hold, std::min<std::chrono::milliseconds>(limit, std::chrono::milliseconds(50)));
            if (progress != nullptr && progress->load() != seen)
            {
                seen = progress->load();
                until = std::chrono::steady_clock::now() + limit;
            }
            if (!state->done && std::chrono::steady_clock::now() >= until)
            {
                state->abandoned = true;
                return false;
            }
        }
        return true;
    }

    bool stuck() const
    {
        const std::lock_guard hold(state->lock);
        return state->abandoned;
    }

private:
    struct State
    {
        std::mutex lock;
        std::condition_variable wake, finished;
        std::function<void()> work;
        bool done = true;
        bool ending = false;
        bool abandoned = false;
    };

    static void loop(State& state)
    {
        std::unique_lock hold(state.lock);
        for (;;)
        {
            state.wake.wait(hold, [&] { return state.ending || state.work != nullptr; });
            if (state.work == nullptr)
                return;

            auto work = std::move(state.work);
            state.work = nullptr;
            hold.unlock();
            work();

            // What it holds goes here too, as it may be what can't be let go of anywhere else.
            work = nullptr;
            hold.lock();
            state.done = true;
            state.finished.notify_all();
        }
    }

    std::shared_ptr<State> state;
    std::thread thread;
};

}
