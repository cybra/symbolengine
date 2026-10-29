#include "ScanWorkerLoop.h"

#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
    #define NOMINMAX
#endif
#include <windows.h>

// After windows.h, which it needs.
#include <ole2.h>

#include <deque>
#include <mutex>

namespace symbolengine::scanWorkerLoop
{

namespace
{

constexpr UINT runTasks = WM_APP + 1;

HWND window = nullptr;
DWORD mainThread = 0;
std::mutex lock;
std::deque<std::function<void()>> tasks;

LRESULT CALLBACK windowProcedure(HWND to, UINT message, WPARAM first, LPARAM second)
{
    if (message != runTasks)
        return DefWindowProcW(to, message, first, second);

    std::deque<std::function<void()>> now;
    {
        const std::lock_guard held(lock);
        now.swap(tasks);
    }
    for (auto& task : now)
        task();
    return 0;
}

}

void begin()
{
    mainThread = GetCurrentThreadId();

    // OLE on the main thread, so plugins can accept dropped files and find COM there.
    OleInitialize(nullptr);

    WNDCLASSW kind{};
    kind.lpfnWndProc = windowProcedure;
    kind.hInstance = GetModuleHandleW(nullptr);
    kind.lpszClassName = L"SymbolEngineScanWorker";
    RegisterClassW(&kind);
    window = CreateWindowExW(0, kind.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, kind.hInstance, nullptr);
}

void post(std::function<void()> function)
{
    {
        const std::lock_guard held(lock);
        tasks.push_back(std::move(function));
    }
    PostMessageW(window, runTasks, 0, 0);
}

bool onMainThread()
{
    return GetCurrentThreadId() == mainThread;
}

namespace
{

// The worker's first visible window with a size, if it has one.
HWND visibleWindow()
{
    HWND found = nullptr;
    EnumWindows(
        [](HWND candidate, LPARAM into) -> BOOL {
            DWORD owner = 0;
            GetWindowThreadProcessId(candidate, &owner);
            RECT area{};
            if (owner != GetCurrentProcessId() || !IsWindowVisible(candidate) || !GetWindowRect(candidate, &area) ||
                area.right <= area.left || area.bottom <= area.top)
                return TRUE;
            *reinterpret_cast<HWND*>(into) = candidate;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&found));
    return found;
}

}

bool showsWindow()
{
    return visibleWindow() != nullptr;
}

void comeForward()
{
    // Asks to be in front, which only the program in front can grant, and puts the window above every other
    // program's meanwhile, which needs no one's leave.
    if (const auto shown = visibleWindow())
    {
        SetForegroundWindow(shown);
        SetWindowPos(shown, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        SetWindowPos(shown, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

void letForward(std::int64_t processId)
{
    AllowSetForegroundWindow(static_cast<DWORD>(processId));
}

void run()
{
    for (;;)
    {
        MSG message;
        if (GetMessageW(&message, nullptr, 0, 0) <= 0)
            continue;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

}
