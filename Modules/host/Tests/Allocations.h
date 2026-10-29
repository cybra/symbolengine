#pragma once

#include <cstdint>

// Counts the allocations and frees the host's code makes on a thread, replacing operator new and delete in the tests'
// program; a plugin module's own allocations go through its own and aren't counted.
namespace allocations
{

/** Counts on the calling thread while it lives. */
struct Counted
{
    Counted();
    ~Counted();

    Counted(const Counted&) = delete;
    Counted& operator=(const Counted&) = delete;
};

/** How many have been counted since the program started. */
std::uint64_t count();

}
