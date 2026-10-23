#include <symbolengine/ui/Native.h>

#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
    #define NOMINMAX
#endif
#include <windows.h>

namespace symbolengine::ui::native
{

bool attach(void* view, void* parent)
{
    if (view == nullptr || parent == nullptr)
        return false;

    const auto child = static_cast<HWND>(view);
    auto style = GetWindowLongPtrW(child, GWL_STYLE);
    style &= ~static_cast<LONG_PTR>(WS_POPUP | WS_CAPTION | WS_THICKFRAME | WS_SYSMENU);
    style |= WS_CHILD;
    SetWindowLongPtrW(child, GWL_STYLE, style);

    if (SetParent(child, static_cast<HWND>(parent)) == nullptr && GetLastError() != ERROR_SUCCESS)
        return false;

    SetWindowPos(child, nullptr, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    return true;
}

void detach(void* view)
{
    if (view != nullptr)
        SetParent(static_cast<HWND>(view), nullptr);
}

void resize(void* view, std::uint32_t width, std::uint32_t height)
{
    if (view != nullptr)
        SetWindowPos(static_cast<HWND>(view), nullptr, 0, 0, static_cast<int>(width), static_cast<int>(height),
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void show(void* view, bool visible)
{
    if (view != nullptr)
        ShowWindow(static_cast<HWND>(view), visible ? SW_SHOWNA : SW_HIDE);
}

}
