#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

// The main thread for the host's tests: the test's own thread, whose run loop the test turns by hand.
class TestMainThread
{
public:
    std::function<void(std::function<void()>)> post()
    {
        return [this](std::function<void()> task) {
            const std::lock_guard held(lock);
            tasks.push_back(std::move(task));
            arrived.notify_all();
        };
    }

    std::function<bool()> isCurrent()
    {
        return [this] { return std::this_thread::get_id() == owner; };
    }

    // One turn of the run loop: the tasks posted before it started, in order.
    int turn()
    {
        std::deque<std::function<void()>> now;
        {
            const std::lock_guard held(lock);
            now.swap(tasks);
        }

        for (auto& task : now)
            task();

        return static_cast<int>(now.size());
    }

    // Turns until nothing is left to run.
    void settle(int limit = 1000)
    {
        for (int i = 0; i < limit && turn() > 0; ++i)
        {
        }
    }

    // Turns until the condition holds, waiting for tasks posted from other threads, up to the limit.
    template <typename Condition>
    bool turnUntil(Condition condition, std::chrono::milliseconds limit = std::chrono::seconds(10))
    {
        const auto until = std::chrono::steady_clock::now() + limit;

        while (!condition())
        {
            if (std::chrono::steady_clock::now() > until)
                return false;

            if (turn() == 0)
            {
                std::unique_lock held(lock);
                arrived.wait_for(held, std::chrono::milliseconds(5), [this] { return !tasks.empty(); });
            }
        }

        return true;
    }

    bool idle()
    {
        const std::lock_guard held(lock);
        return tasks.empty();
    }

private:
    std::thread::id owner = std::this_thread::get_id();
    std::mutex lock;
    std::condition_variable arrived;
    std::deque<std::function<void()>> tasks;
};
