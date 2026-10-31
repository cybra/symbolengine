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

void turn()
{
    MSG message;
    for (int handled = 0; handled < 16 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++handled)
    {
        if (message.message == WM_QUIT)
        {
            PostQuitMessage(static_cast<int>(message.wParam));
            return;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}
}
