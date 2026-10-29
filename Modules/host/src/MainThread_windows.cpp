#include "MainThread.h"

#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
    #define NOMINMAX
#endif
#include <windows.h>

#include <ole2.h>

namespace symbolengine::mainThread
{

// Kept for the life of the process, as plugins may register for dropped files at any time.
void prepare()
{
    static const auto prepared = OleInitialize(nullptr);
    static_cast<void>(prepared);
}

}
