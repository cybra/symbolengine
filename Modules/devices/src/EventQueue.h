#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace symbolengine::devices::detail
{

/** Runs what is posted to it in turn on a thread of its own: where a stream's and the device lists' events are
    delivered when the program gives no thread of its own. A handler may destroy what posted to it. */
class EventQueue
{
public:
    static std::shared_ptr<EventQueue> make()
    {
        auto queue = std::shared_ptr<EventQueue>(new EventQueue);
        queue->thread = std::thread([queue] { queue->run(); });
        return queue;
    }

    void post(std::function<void()> work)
    {
        {
            const std::lock_guard hold(lock);
            if (stopping)
                return;
            waiting.push_back(std::move(work));
        }
        wake.notify_one();
    }

    /** Runs what is already posted and ends the thread; from the queue's own thread it lets the thread end alone. */
    void stop()
    {
        {
            const std::lock_guard hold(lock);
            stopping = true;
        }
        wake.notify_one();
        if (std::this_thread::get_id() == thread.get_id())
            thread.detach();
        else if (thread.joinable())
            thread.join();
    }

    ~EventQueue()
    {
        if (thread.joinable())
            thread.detach();
    }

private:
    EventQueue() = default;

    void run()
    {
        std::unique_lock hold(lock);
        for (;;)
        {
            wake.wait(hold, [this] { return stopping || !waiting.empty(); });
            if (waiting.empty() && stopping)
                break;
            auto next = std::move(waiting.front());
            waiting.pop_front();
            hold.unlock();
            next();
            hold.lock();
        }
    }

    std::mutex lock;
    std::condition_variable wake;
    std::deque<std::function<void()>> waiting;
    bool stopping = false;
    std::thread thread;
};

}
