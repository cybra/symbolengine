#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace symbolengine::plugin::detail
{

/** A change the editor made to a parameter, for the host to be told of. */
struct EditorChange
{
    enum class Kind : std::uint8_t
    {
        begin,   // a gesture starts, such as a drag
        value,
        end
    };

    Kind kind = Kind::value;
    std::uint32_t id = 0;
    double value = 0.0;
};

/** Changes from the editor, made on the message thread, to whichever thread next tells the host of them: the audio
    thread while the plugin processes, or the thread the host flushes on while it doesn't, which never happen at once.
    One thread adds and one takes, without locking. When it is full, a change is dropped: its value is still set, and
    the host only misses it as automation. */
class EditorChanges
{
public:
    bool push(const EditorChange& change) noexcept
    {
        const auto at = writeAt.load(std::memory_order_relaxed);
        const auto next = (at + 1) % capacity;
        if (next == readAt.load(std::memory_order_acquire))
            return false;
        items[at] = change;
        writeAt.store(next, std::memory_order_release);
        return true;
    }

    template <typename Take>
    void drain(Take&& take) noexcept
    {
        auto at = readAt.load(std::memory_order_relaxed);
        const auto last = writeAt.load(std::memory_order_acquire);
        while (at != last)
        {
            take(items[at]);
            at = (at + 1) % capacity;
        }
        readAt.store(at, std::memory_order_release);
    }

private:
    static constexpr std::size_t capacity = 4096;
    std::array<EditorChange, capacity> items{};
    std::atomic<std::size_t> readAt{0};
    std::atomic<std::size_t> writeAt{0};
};

}
