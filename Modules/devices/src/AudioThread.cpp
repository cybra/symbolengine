#include "symbolengine/devices/AudioThread.h"

#include "AudioThreadMarker.h"

#include <atomic>

namespace symbolengine::audioThread
{

namespace
{
thread_local bool inside = false;
std::atomic<std::uint64_t> counted{0};
}

bool isCurrent() noexcept
{
    return inside;
}

void noteAllocation() noexcept
{
    counted.fetch_add(1, std::memory_order_relaxed);
}

std::uint64_t allocations() noexcept
{
    return counted.load(std::memory_order_relaxed);
}

namespace detail
{
Marker::Marker() noexcept : before(inside)
{
    inside = true;
}

Marker::~Marker()
{
    inside = before;
}
}

}
