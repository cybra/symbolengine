#pragma once

#include <cstdint>

namespace symbolengine
{

/** Runs the function and, on Windows, catches a structured exception from it, as a plugin's fault: an access
    violation, an illegal instruction, a stack overflow. A C++ exception passes on for the caller to catch. False if it
    faulted, with the fault's code; elsewhere it only runs the function, as nothing can carry on after a signal. */
bool guarded(void (*body)(void*), void* context, std::uint32_t& fault);

}
