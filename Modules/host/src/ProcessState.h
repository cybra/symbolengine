#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace symbolengine
{

/** What belongs to the whole process that a plugin's code can change and not put back: the floating-point control
    register's modes, the C locale, the working folder and, on a Mac, mouse-event coalescing. */
struct ProcessState
{
    std::uint64_t floatingPoint = 0;
    std::string locale;
    std::filesystem::path folder;
    bool coalescesMouse = true;

    static ProcessState now();

    /** Puts back whatever changed since this was taken, giving whether anything had. */
    bool putBack() const;
};

/** The calling thread's floating-point modes, rounding, exception masks and flushing denormals, and setting them. */
std::uint64_t floatingPointModes() noexcept;
void setFloatingPointModes(std::uint64_t modes) noexcept;

/** On a Mac, whether mouse events are coalesced, and setting it; elsewhere always true and nothing. */
bool mouseCoalescing();
void setMouseCoalescing(bool on);

}
