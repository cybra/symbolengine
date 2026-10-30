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

bool floatsAbove(void* window)
{
    return (GetWindowLongPtrW(static_cast<HWND>(window), GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
}

// A window of the process's own, as a question or a plugin's own window is, shown far off every display without
// taking the keyboard.
void* showOtherWindow()
{
    const auto window = CreateWindowExW(0, L"STATIC", L"Other", WS_POPUP | WS_CAPTION, -30000, -30000, 200, 100,
                                        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ShowWindow(window, SW_SHOWNOACTIVATE);
    return window;
}

void closeOtherWindow(void* window)
{
    DestroyWindow(static_cast<HWND>(window));
}

// The window the plugin's view is in, or the plugin's own view there, which a key pressed in the plugin goes to.
static HWND keyTarget(symbolengine::PluginWindow& pluginWindow)
{
    const auto container = GetWindow(static_cast<HWND>(pluginWindow.nativeHandle()), GW_CHILD);
    const auto own = container != nullptr ? GetWindow(container, GW_CHILD) : nullptr;
    const auto focus = GetFocus();
    if (focus != nullptr && container != nullptr && IsChild(container, focus))
        return focus;
    return own != nullptr ? own : container;
}

// A key pressed and let go of in the window, as Windows posts it there: the virtual key, and a press marked as a repeat
// when it is one.
void pressKey(symbolengine::PluginWindow& pluginWindow, char32_t key, bool repeat, bool)
{
    const auto target = keyTarget(pluginWindow);
    const auto virtualKey = static_cast<WPARAM>(key == U' ' ? VK_SPACE : LOBYTE(VkKeyScanW(static_cast<wchar_t>(key))));
    const auto scan = static_cast<LPARAM>(MapVirtualKeyW(static_cast<UINT>(virtualKey), MAPVK_VK_TO_VSC)) << 16;
    PostMessageW(target, WM_KEYDOWN, virtualKey, 1 | scan | (repeat ? LPARAM(1) << 30 : 0));
    PostMessageW(target, WM_KEYUP, virtualKey, 1 | scan | (LPARAM(1) << 30) | (LPARAM(1) << 31));
    turnSystemLoop();
}

// A text field in the window being typed in.
void* typeIn(symbolengine::PluginWindow& pluginWindow)
{
    const auto container = GetWindow(static_cast<HWND>(pluginWindow.nativeHandle()), GW_CHILD);
    const auto field = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE, 0, 0, 100, 20, container, nullptr,
                                       GetModuleHandleW(nullptr), nullptr);
    SetFocus(field);
    return field;
}

void stopTyping(void* field)
{
    SetFocus(nullptr);
    DestroyWindow(static_cast<HWND>(field));
}
