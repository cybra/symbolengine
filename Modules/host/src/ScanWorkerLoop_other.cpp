#include "ScanWorkerLoop.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace symbolengine::scanWorkerLoop
{

namespace
{

std::thread::id mainThread;
std::mutex lock;
std::condition_variable arrived;
std::deque<std::function<void()>> tasks;

}

void begin()
{
    mainThread = std::this_thread::get_id();
}

void post(std::function<void()> function)
{
    const std::lock_guard held(lock);
    tasks.push_back(std::move(function));
    arrived.notify_one();
}

bool onMainThread()
{
    return std::this_thread::get_id() == mainThread;
}

bool showsWindow()
{
    return false;
}

void comeForward() {}

void letForward(std::int64_t) {}

void run()
{
    for (;;)
    {
        std::function<void()> task;
        {
            std::unique_lock held(lock);
            arrived.wait(held, [] { return !tasks.empty(); });
            task = std::move(tasks.front());
            tasks.pop_front();
        }
        task();
    }
}

}
