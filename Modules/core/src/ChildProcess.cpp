#include "ProcessChannel.h"

#include <condition_variable>
#include <mutex>
#include <thread>

namespace symbolengine
{

namespace
{

// Each frame is the length of what follows, as 4 bytes little-endian, then its kind, then the message's bytes.
enum class Frame : unsigned char
{
    message = 0,
    heartbeat = 1,
};

constexpr std::size_t headerSize = 5;

}

struct ProcessLink::State
{
    std::unique_ptr<Native> native;
    std::function<void(std::string)> message;
    std::function<void()> lostCallback;
    Timing timing;

    std::mutex writing;
    std::atomic<bool> started{false}, lost{false}, closing{false}, heard{false};
    std::atomic<int> beatsSinceHeard{0};

    std::mutex sleeping;
    std::condition_variable wake;
    std::thread reader, heartbeat, beatSender, watcher;

    bool sendFrame(Frame kind, std::string_view bytes, bool wait)
    {
        if (lost || closing)
            return false;

        const auto length = static_cast<std::uint32_t>(bytes.size() + 1);
        const char header[headerSize] = {static_cast<char>(length & 0xFF), static_cast<char>((length >> 8) & 0xFF),
                                         static_cast<char>((length >> 16) & 0xFF),
                                         static_cast<char>((length >> 24) & 0xFF), static_cast<char>(kind)};

        std::unique_lock held(writing, std::defer_lock);

        // A heartbeat never waits behind a long message: the message going out says as much.
        if (wait)
            held.lock();
        else if (!held.try_lock())
            return true;

        // Failing here is left to the reader to say, which it does as the channel shuts: whoever is sending may hold what
        // the program's own function for a lost link wants.
        if (!native->write(header, headerSize) || !native->write(bytes.data(), bytes.size()))
        {
            held.unlock();
            native->shutDown();
            return false;
        }

        return true;
    }

    // Wakes whatever waits on the link's timing, with no chance of its missing this between looking and sleeping.
    void wakeAll()
    {
        {
            const std::lock_guard held(sleeping);
        }
        wake.notify_all();
    }

    // The other end has gone: said once, unless this side closed the link itself.
    void lose()
    {
        if (lost.exchange(true))
            return;

        native->shutDown();
        wakeAll();

        if (!closing && lostCallback != nullptr)
            lostCallback();
    }

    void read()
    {
        native->beginReading();
        char header[headerSize];
        std::string bytes;

        while (native->read(header, headerSize))
        {
            const auto length = static_cast<std::uint32_t>(static_cast<unsigned char>(header[0])) |
                                static_cast<std::uint32_t>(static_cast<unsigned char>(header[1])) << 8 |
                                static_cast<std::uint32_t>(static_cast<unsigned char>(header[2])) << 16 |
                                static_cast<std::uint32_t>(static_cast<unsigned char>(header[3])) << 24;
            const auto kind = static_cast<unsigned char>(header[4]);

            if (length == 0 || length - 1 > maximumMessage || kind > static_cast<unsigned char>(Frame::heartbeat))
                break;

            bytes.resize(length - 1);

            if (!bytes.empty() && !native->read(bytes.data(), bytes.size()))
                break;

            beatsSinceHeard = 0;
            heard = true;

            if (kind == static_cast<unsigned char>(Frame::message) && message != nullptr)
                message(std::move(bytes));

            bytes = {};
        }

        native->endReading();
        lose();
    }

    void beat()
    {
        while (!lost && !closing)
        {
            {
                std::unique_lock held(sleeping);
                wake.wait_for(held, timing.heartbeat, [this] { return lost || closing; });
            }

            if (lost || closing)
                return;

            // Counted in this side's own beats, so time the computer spends asleep, when no thread runs, isn't silence,
            // and only once the other end has been heard from, so a program slow to start isn't taken for gone.
            if (heard && ++beatsSinceHeard > timing.silentBeats)
            {
                lose();
                return;
            }
        }
    }

    // Sent from a thread of their own, as a channel filled by a child not reading holds up a write, which mustn't stop
    // the beats being counted.
    void sendBeats()
    {
        while (!lost && !closing)
        {
            {
                std::unique_lock held(sleeping);
                wake.wait_for(held, timing.heartbeat, [this] { return lost || closing; });
            }

            if (lost || closing)
                return;

            sendFrame(Frame::heartbeat, {}, false);
        }
    }

    void watch()
    {
        if (native->waitForParent(closing))
            lose();
    }

    void join(std::thread& thread)
    {
        if (!thread.joinable())
            return;

        if (thread.get_id() == std::this_thread::get_id())
            thread.detach();
        else
            thread.join();
    }
};

ProcessLink::ProcessLink(std::unique_ptr<Native> native) : state(std::make_shared<State>())
{
    state->native = std::move(native);
}

ProcessLink::~ProcessLink()
{
    close();
}

void ProcessLink::onMessage(std::function<void(std::string)> callback)
{
    state->message = std::move(callback);
}

void ProcessLink::onLost(std::function<void()> callback)
{
    state->lostCallback = std::move(callback);
}

void ProcessLink::start()
{
    start(Timing{});
}

void ProcessLink::start(Timing timing)
{
    if (state->started.exchange(true) || state->closing)
        return;

    state->timing = timing;
    state->reader = std::thread([held = state] { held->read(); });
    state->heartbeat = std::thread([held = state] { held->beat(); });
    state->beatSender = std::thread([held = state] { held->sendBeats(); });

    if (state->native->watchesParent())
        state->watcher = std::thread([held = state] { held->watch(); });
}

bool ProcessLink::send(std::string_view message)
{
    if (message.size() > maximumMessage)
        return false;

    return state->sendFrame(Frame::message, message, true);
}

bool ProcessLink::isLost() const
{
    return state->lost && !state->closing;
}

void ProcessLink::close()
{
    if (state->closing.exchange(true))
        return;

    state->native->shutDown();
    state->wakeAll();
    state->join(state->reader);
    state->join(state->heartbeat);
    state->join(state->beatSender);
    state->join(state->watcher);
}

}
