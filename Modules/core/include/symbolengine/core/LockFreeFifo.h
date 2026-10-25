#pragma once

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <vector>

namespace symbolengine
{

/** Items passed from a thread that writes to a thread that reads, neither ever locking or waiting on the other, as an
    audio thread needs: samples or messages from an audio device's thread, or to it. It holds exactly the capacity it
    is made with. One thread writes at a time and one reads at a time, which a debug build checks; the two may be
    different threads from one run of a device to the next. */
template <typename T>
class LockFreeFifo
{
public:
    explicit LockFreeFifo(std::size_t capacity) : items(capacity + 1) {}

    LockFreeFifo(const LockFreeFifo&) = delete;
    LockFreeFifo& operator=(const LockFreeFifo&) = delete;

    std::size_t capacity() const noexcept { return items.size() - 1; }

    /** How many items wait to be read; from either thread, as of the moment it is asked. */
    std::size_t readable() const noexcept
    {
        return distance(readAt.load(std::memory_order_acquire), writeAt.load(std::memory_order_acquire));
    }

    /** How many items there is room for. */
    std::size_t writable() const noexcept { return capacity() - readable(); }

    /** Writes as many of count items as there is room for, in order, and returns how many it wrote. */
    std::size_t write(const T* from, std::size_t count) noexcept
    {
        const Using one(writing);
        const auto at = writeAt.load(std::memory_order_relaxed);
        count = std::min(count, capacity() - distance(readAt.load(std::memory_order_acquire), at));
        const auto first = std::min(count, items.size() - at);
        std::copy(from, from + first, items.begin() + static_cast<std::ptrdiff_t>(at));
        std::copy(from + first, from + count, items.begin());
        writeAt.store((at + count) % items.size(), std::memory_order_release);
        return count;
    }

    bool push(const T& item) noexcept { return write(&item, 1) == 1; }

    /** Reads up to count items, oldest first, and returns how many it read. */
    std::size_t read(T* to, std::size_t count) noexcept
    {
        const Using one(reading);
        const auto at = readAt.load(std::memory_order_relaxed);
        count = std::min(count, distance(at, writeAt.load(std::memory_order_acquire)));
        const auto first = std::min(count, items.size() - at);
        std::copy(items.begin() + static_cast<std::ptrdiff_t>(at),
                  items.begin() + static_cast<std::ptrdiff_t>(at + first), to);
        std::copy(items.begin(), items.begin() + static_cast<std::ptrdiff_t>(count - first), to + first);
        readAt.store((at + count) % items.size(), std::memory_order_release);
        return count;
    }

    bool pop(T& item) noexcept { return read(&item, 1) == 1; }

    /** Passes over up to count items without reading them, on the reading side, and returns how many it passed. */
    std::size_t skip(std::size_t count) noexcept
    {
        const Using one(reading);
        const auto at = readAt.load(std::memory_order_relaxed);
        count = std::min(count, distance(at, writeAt.load(std::memory_order_acquire)));
        readAt.store((at + count) % items.size(), std::memory_order_release);
        return count;
    }

    /** Empties it, while neither thread uses it. */
    void reset() noexcept
    {
        readAt.store(0, std::memory_order_relaxed);
        writeAt.store(0, std::memory_order_release);
    }

private:
    std::size_t distance(std::size_t from, std::size_t to) const noexcept
    {
        return (to + items.size() - from) % items.size();
    }

    // Marks one side in use for as long as it lives, and in a debug build stops at a second thread using that side
    // at the same time, which would lose or repeat items.
    struct Using
    {
#ifndef NDEBUG
        explicit Using(std::atomic<bool>& flag) noexcept : side(flag)
        {
            [[maybe_unused]] const bool before = side.exchange(true, std::memory_order_acquire);
            assert(!before && "two threads at once on one side of a LockFreeFifo");
        }
        ~Using() { side.store(false, std::memory_order_release); }
        std::atomic<bool>& side;
#else
        explicit Using(std::atomic<bool>&) noexcept {}
#endif
    };

    // One more item than the capacity, so a full fifo and an empty one are told apart.
    std::vector<T> items;
    std::atomic<std::size_t> readAt{0}, writeAt{0};
    std::atomic<bool> writing{false}, reading{false};
};

}
