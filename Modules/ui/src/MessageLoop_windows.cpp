#include <symbolengine/ui/MessageLoop.h>

#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
    #define NOMINMAX
#endif
#include <windows.h>

namespace symbolengine::ui::messageLoop
{

// Messages to a window the loop dispatches, which any loop on this thread does.
void callSoon(std::function<void()> function)
{
    callAsync(std::move(function));
}

bool runUntil(const std::function<bool()>& condition, std::chrono::milliseconds limit)
{
    const auto until = std::chrono::steady_clock::now() + limit;
    while (!condition())
    {
        if (std::chrono::steady_clock::now() >= until)
            return false;

        MSG message;
        if (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            // The program is quitting, which the loop that runs it has to hear.
            if (message.message == WM_QUIT)
            {
                PostQuitMessage(static_cast<int>(message.wParam));
                return false;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        else
            MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
    return true;
}

}
