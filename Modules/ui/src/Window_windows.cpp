#include <symbolengine/ui/Window.h>

#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
    #define NOMINMAX
#endif
#include <windows.h>

#include <dwmapi.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdint>
#include <string>

namespace symbolengine::ui
{

namespace
{
// The close button's call, posted so it comes once the click is over.
constexpr UINT closeButtonPressed = WM_APP + 0x5E01;

// Window attributes only the newer SDKs name: a dark title bar, from Windows 10's 20H1 on, and Windows 11's title bar
// and title colors.
constexpr DWORD darkTitleBar = 20, captionColor = 35, captionTextColor = 36;

constexpr DWORD windowStyle(bool resizable, bool maximizable)
{
    return WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN | (resizable ? WS_THICKFRAME : 0u) |
           (resizable && maximizable ? WS_MAXIMIZEBOX : 0u);
}

std::wstring wide(const std::string& text)
{
    if (text.empty())
        return {};
    const auto length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length);
    return result;
}

int scaled(int units, UINT dpi)
{
    return MulDiv(units, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
}

int unscaled(int pixels, UINT dpi)
{
    return MulDiv(pixels, USER_DEFAULT_SCREEN_DPI, static_cast<int>(dpi));
}

HMONITOR mainMonitor()
{
    return MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
}

RECT workAreaOf(HMONITOR monitor)
{
    MONITORINFO info{};
    info.cbSize = sizeof info;
    if (!GetMonitorInfoW(monitor, &info))
        return {0, 0, 1280, 800};
    return info.rcWork;
}

// Takes a view out of the window, leaving it a hidden window of its own: a child given no parent is made a popup, as
// Windows asks, or the desktop becomes its parent.
void release(HWND view)
{
    ShowWindow(view, SW_HIDE);
    SetParent(view, nullptr);
    const auto style = GetWindowLongPtrW(view, GWL_STYLE);
    SetWindowLongPtrW(view, GWL_STYLE, (style & ~static_cast<LONG_PTR>(WS_CHILD)) | static_cast<LONG_PTR>(WS_POPUP));
}

// Whether the computer is set to dark apps, as Settings, Personalization, Colors has it.
bool systemIsDark()
{
    DWORD light = 1, size = sizeof light;
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
    return light == 0;
}
}

struct Window::Impl
{
    Window* owner = nullptr;
    HWND window = nullptr;
    HWND content = nullptr;
    HBRUSH background = nullptr;
    DWORD style = 0;
    Size minimum{};
    Bounds restored{};
    Appearance appearance = Appearance::system;

    UINT dpi() const
    {
        return window != nullptr ? GetDpiForWindow(window) : USER_DEFAULT_SCREEN_DPI;
    }

    // The frame around a client area of the size given, in the window's own pixels.
    RECT frameFor(RECT client, UINT forDpi) const
    {
        AdjustWindowRectExForDpi(&client, style, FALSE, 0, forDpi);
        return client;
    }

    RECT clientOnScreen() const
    {
        RECT client{};
        GetClientRect(window, &client);
        MapWindowPoints(window, nullptr, reinterpret_cast<POINT*>(&client), 2);
        return client;
    }

    // Where it was, kept as it moves while it is neither maximized nor minimized.
    void keepRestored()
    {
        if (IsIconic(window) || IsZoomed(window))
            return;
        const auto client = clientOnScreen();
        restored = {client.left, client.top, client.right - client.left, client.bottom - client.top};
    }

    void fillWithContent() const
    {
        if (content == nullptr)
            return;
        RECT client{};
        GetClientRect(window, &client);
        SetWindowPos(content, nullptr, 0, 0, client.right, client.bottom, SWP_NOZORDER | SWP_NOACTIVATE);
    }

    void applyAppearance() const
    {
        const BOOL dark = appearance == Appearance::dark || (appearance == Appearance::system && systemIsDark());
        DwmSetWindowAttribute(window, darkTitleBar, &dark, sizeof dark);
    }

    // A frame held to the work area of the monitor it overlaps most, or the main monitor when it overlaps none.
    static RECT fitted(RECT frame)
    {
        auto monitor = MonitorFromRect(&frame, MONITOR_DEFAULTTONULL);
        const auto area = workAreaOf(monitor != nullptr ? monitor : mainMonitor());
        const auto width = std::min(frame.right - frame.left, area.right - area.left);
        const auto height = std::min(frame.bottom - frame.top, area.bottom - area.top);
        const auto left = std::clamp(frame.left, area.left, area.right - width);
        const auto top = std::clamp(frame.top, area.top, area.bottom - height);
        return {left, top, left + width, top + height};
    }

    void place(const RECT& frame) const
    {
        SetWindowPos(window, nullptr, frame.left, frame.top, frame.right - frame.left, frame.bottom - frame.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }

    static LRESULT CALLBACK procedure(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == WM_NCCREATE)
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams));

        auto* impl = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (impl == nullptr || impl->window == nullptr)
            return DefWindowProcW(hwnd, message, wParam, lParam);

        auto& owner = *impl->owner;
        switch (message)
        {
            case WM_CLOSE:
                PostMessageW(hwnd, closeButtonPressed, 0, 0);
                return 0;

            // A copy is called, as the program may destroy the window, and the function with it.
            case closeButtonPressed:
                if (auto pressed = owner.onCloseButton)
                    pressed();
                return 0;

            case WM_ACTIVATE:
                if (owner.onActiveChanged)
                    owner.onActiveChanged(LOWORD(wParam) != WA_INACTIVE);
                break;

            case WM_SETFOCUS:
                if (impl->content != nullptr)
                    SetFocus(impl->content);
                return 0;

            case WM_SIZE:
                impl->fillWithContent();
                impl->keepRestored();
                if (owner.onMovedOrResized)
                    owner.onMovedOrResized();
                break;

            case WM_MOVE:
                impl->keepRestored();
                if (owner.onMovedOrResized)
                    owner.onMovedOrResized();
                break;

            case WM_GETMINMAXINFO:
            {
                const auto dpi = impl->dpi();
                const auto frame = impl->frameFor({0, 0, scaled(impl->minimum.width, dpi), scaled(impl->minimum.height, dpi)}, dpi);
                auto* limits = reinterpret_cast<MINMAXINFO*>(lParam);
                limits->ptMinTrackSize = {frame.right - frame.left, frame.bottom - frame.top};
                return 0;
            }

            // Moved to a screen of another scale, it takes the size Windows suggests, the same size on the new screen.
            case WM_DPICHANGED:
                impl->place(*reinterpret_cast<const RECT*>(lParam));
                return 0;

            case WM_ERASEBKGND:
            {
                RECT client{};
                GetClientRect(hwnd, &client);
                FillRect(reinterpret_cast<HDC>(wParam), &client, impl->background);
                return 1;
            }

            case WM_SETTINGCHANGE:
                if (impl->appearance == Appearance::system && lParam != 0 &&
                    lstrcmpW(reinterpret_cast<const wchar_t*>(lParam), L"ImmersiveColorSet") == 0)
                    impl->applyAppearance();
                break;

            default:
                break;
        }

        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    // The window class, registered once under a name of its own, so two programs' copies of SymbolEngine in one
    // process, as plugins in a host, never share one.
    static const wchar_t* windowClass()
    {
        static const std::wstring name = [] {
            HMODULE module = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(&procedure), &module);
            auto className = L"SymbolEngineWindow_" + std::to_wstring(reinterpret_cast<std::uintptr_t>(&procedure));

            WNDCLASSEXW windowClass{};
            windowClass.cbSize = sizeof windowClass;
            windowClass.lpfnWndProc = procedure;
            windowClass.hInstance = module;
            windowClass.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
            windowClass.lpszClassName = className.c_str();

            // The program's own icons, the first in its file, for the title bar and the taskbar.
            wchar_t file[MAX_PATH] = {};
            if (GetModuleFileNameW(module, file, MAX_PATH) > 0)
                ExtractIconExW(file, 0, &windowClass.hIcon, &windowClass.hIconSm, 1);
            RegisterClassExW(&windowClass);
            return className;
        }();
        return name.c_str();
    }
};

Window::Window(Options options) : impl(std::make_unique<Impl>())
{
    auto& w = *impl;
    w.owner = this;
    w.minimum = options.minimumSize;
    w.style = windowStyle(options.resizable, options.maximizable);
    w.background = CreateSolidBrush(RGB((options.background >> 16) & 0xFF, (options.background >> 8) & 0xFF,
                                        options.background & 0xFF));

    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&Impl::procedure), &module);

    // Made on the main screen, whose scale its size then takes.
    const auto area = workAreaOf(mainMonitor());
    w.window = CreateWindowExW(0, Impl::windowClass(), wide(options.title).c_str(), w.style, area.left, area.top, 100, 100,
                               nullptr, nullptr, module, impl.get());
    w.applyAppearance();
    setSize(options.size);
    center();
    w.keepRestored();
}

Window::~Window()
{
    auto& w = *impl;
    if (w.content != nullptr)
        release(w.content);

    SetWindowLongPtrW(w.window, GWLP_USERDATA, 0);
    DestroyWindow(w.window);
    DeleteObject(w.background);
}

void Window::setContent(void* view)
{
    auto& w = *impl;
    if (w.content != nullptr)
        release(w.content);

    w.content = static_cast<HWND>(view);
    if (w.content == nullptr)
        return;

    auto style = GetWindowLongPtrW(w.content, GWL_STYLE);
    style &= ~static_cast<LONG_PTR>(WS_POPUP | WS_CAPTION | WS_THICKFRAME | WS_SYSMENU);
    style |= WS_CHILD | WS_VISIBLE;
    SetWindowLongPtrW(w.content, GWL_STYLE, style);
    SetParent(w.content, w.window);
    w.fillWithContent();
}

void Window::setTitle(const std::string& title)
{
    SetWindowTextW(impl->window, wide(title).c_str());
}

void Window::setVisible(bool visible)
{
    ShowWindow(impl->window, visible ? SW_SHOWNA : SW_HIDE);
}

bool Window::isVisible() const
{
    return IsWindowVisible(impl->window) != FALSE;
}

void Window::toFront()
{
    const auto window = impl->window;
    ShowWindow(window, IsIconic(window) ? SW_RESTORE : SW_SHOW);
    BringWindowToTop(window);
    SetForegroundWindow(window);
}

bool Window::isActive() const
{
    return GetForegroundWindow() == impl->window;
}

bool Window::isMinimized() const
{
    return IsIconic(impl->window) != FALSE;
}

void Window::setAlwaysOnTop(bool onTop)
{
    SetWindowPos(impl->window, onTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void Window::setSeeThrough(bool seeThrough)
{
    // Cloaked, a window is drawn as usual, its web view included, but not shown.
    const BOOL cloak = seeThrough ? TRUE : FALSE;
    DwmSetWindowAttribute(impl->window, DWMWA_CLOAK, &cloak, sizeof cloak);
}

void Window::setAppearance(Appearance appearance)
{
    impl->appearance = appearance;
    impl->applyAppearance();
}

Size Window::size() const
{
    RECT client{};
    GetClientRect(impl->window, &client);
    const auto dpi = impl->dpi();
    return {unscaled(client.right, dpi), unscaled(client.bottom, dpi)};
}

void Window::setSize(Size size)
{
    auto& w = *impl;
    const auto dpi = w.dpi();
    const auto client = w.clientOnScreen();
    const auto frame = w.frameFor({client.left, client.top, client.left + scaled(std::max(size.width, w.minimum.width), dpi),
                                   client.top + scaled(std::max(size.height, w.minimum.height), dpi)},
                                  dpi);
    SetWindowPos(w.window, nullptr, 0, 0, frame.right - frame.left, frame.bottom - frame.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

Size Window::workArea() const
{
    const auto area = workAreaOf(MonitorFromWindow(impl->window, MONITOR_DEFAULTTOPRIMARY));
    const auto dpi = impl->dpi();
    return {unscaled(area.right - area.left, dpi), unscaled(area.bottom - area.top, dpi)};
}

Bounds Window::bounds() const
{
    const auto client = impl->clientOnScreen();
    return {client.left, client.top, client.right - client.left, client.bottom - client.top};
}

void Window::setBounds(Bounds bounds)
{
    auto& w = *impl;
    const auto dpi = w.dpi();
    bounds.width = std::max(bounds.width, scaled(w.minimum.width, dpi));
    bounds.height = std::max(bounds.height, scaled(w.minimum.height, dpi));
    w.place(Impl::fitted(w.frameFor({bounds.x, bounds.y, bounds.x + bounds.width, bounds.y + bounds.height}, dpi)));
}

void Window::center()
{
    auto& w = *impl;
    RECT frame{};
    GetWindowRect(w.window, &frame);
    const auto area = workAreaOf(mainMonitor());
    const auto width = frame.right - frame.left, height = frame.bottom - frame.top;
    const auto left = area.left + (area.right - area.left - width) / 2;
    const auto top = area.top + (area.bottom - area.top - height) / 2;
    w.place(Impl::fitted({left, top, left + width, top + height}));
}

void Window::setMaximized(bool maximized)
{
    if (maximized != isMaximized())
        ShowWindow(impl->window, maximized ? SW_MAXIMIZE : SW_RESTORE);
}

bool Window::isMaximized() const
{
    return IsZoomed(impl->window) != FALSE;
}

Bounds Window::restoredBounds() const
{
    return impl->restored;
}

void Window::setTitleBarColor(std::uint32_t background, std::uint32_t text)
{
    const COLORREF caption = RGB((background >> 16) & 0xFF, (background >> 8) & 0xFF, background & 0xFF);
    const COLORREF title = RGB((text >> 16) & 0xFF, (text >> 8) & 0xFF, text & 0xFF);
    DwmSetWindowAttribute(impl->window, captionColor, &caption, sizeof caption);
    DwmSetWindowAttribute(impl->window, captionTextColor, &title, sizeof title);
}

void Window::focusContent()
{
    if (impl->content != nullptr)
        SetFocus(impl->content);
}

void* Window::nativeHandle() const
{
    return impl->window;
}

}
