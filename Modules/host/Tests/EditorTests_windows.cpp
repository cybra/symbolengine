// What the editor tests do with the system's windows on Windows. The windows are never shown.

#include <symbolengine/host/PluginWindow.h>

#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
    #define NOMINMAX
#endif
#include <windows.h>

#include <cmath>

// Runs the messages waiting for the thread's windows.
void turnSystemLoop()
{
    MSG message{};

    for (int i = 0; i < 1000 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++i)
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

// The user dragging the window's bottom right corner to a size in pixels: the system asks the window what size it
// takes, as at each step of a drag, and resizes it to that. Gives its size after, in pixels.
symbolengine::ui::Size dragWindow(symbolengine::PluginWindow& pluginWindow, symbolengine::ui::Size to)
{
    const auto window = static_cast<HWND>(pluginWindow.nativeHandle());
    RECT outer{}, client{};
    GetWindowRect(window, &outer);
    GetClientRect(window, &client);
    const int edgesWide = (outer.right - outer.left) - client.right;
    const int edgesHigh = (outer.bottom - outer.top) - client.bottom;
    RECT dragged{outer.left, outer.top, outer.left + edgesWide + to.width, outer.top + edgesHigh + to.height};
    SendMessageW(window, WM_SIZING, WMSZ_BOTTOMRIGHT, reinterpret_cast<LPARAM>(&dragged));
    SetWindowPos(window, nullptr, 0, 0, dragged.right - dragged.left, dragged.bottom - dragged.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    GetClientRect(window, &client);
    return {client.right, client.bottom};
}

// A window's handle stays the same until it is destroyed, after which it names no window.
void* keepWindow(void* window)
{
    return window;
}

void letGoOfWindow(void*) {}

// Whether the window is still there and holds the views put in it.
bool holdsViews(void* window)
{
    const auto handle = static_cast<HWND>(window);
    return IsWindow(handle) && GetWindow(handle, GW_CHILD) != nullptr;
}

void beginWindowMove(void* window)
{
    SendMessageW(static_cast<HWND>(window), WM_ENTERSIZEMOVE, 0, 0);
}

void endWindowMove(void* window)
{
    SendMessageW(static_cast<HWND>(window), WM_EXITSIZEMOVE, 0, 0);
}

// The system telling the window its screen's scale changed: the new DPI, with a place holding its client area scaled to
// it inside the frame it has, a test's stand-in for a real screen of that scale.
void rescaleWindow(void* window, double scale)
{
    const auto handle = static_cast<HWND>(window);
    const auto dpi = static_cast<UINT>(std::lround(scale * 96.0));
    const auto before = static_cast<int>(GetDpiForWindow(handle));
    RECT outer{}, client{};
    GetWindowRect(handle, &outer);
    GetClientRect(handle, &client);
    const int edgesWide = (outer.right - outer.left) - client.right;
    const int edgesHigh = (outer.bottom - outer.top) - client.bottom;
    const RECT suggested{outer.left, outer.top,
                         outer.left + edgesWide + MulDiv(client.right, static_cast<int>(dpi), before),
                         outer.top + edgesHigh + MulDiv(client.bottom, static_cast<int>(dpi), before)};
    SendMessageW(handle, WM_DPICHANGED, MAKEWPARAM(dpi, dpi), reinterpret_cast<LPARAM>(&suggested));
}
