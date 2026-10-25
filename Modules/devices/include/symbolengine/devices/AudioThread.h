#pragma once

#include <cstdint>

namespace symbolengine::audioThread
{

/** Whether the calling thread is inside a device's cycle, where nothing may allocate, free, lock or wait. */
bool isCurrent() noexcept;

/** Counts an allocation or a free made inside a device's cycle; AudioThreadChecks.h counts them with this. */
void noteAllocation() noexcept;

/** How many have been counted since the program started. */
std::uint64_t allocations() noexcept;

}
