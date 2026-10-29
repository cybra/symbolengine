#include "PluginWindowNative.h"

#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
    #define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <string>

namespace symbolengine::pluginWindowNative
{

namespace
{

// The view leaves its whole area to the plugin's window in it, so the program never paints over the plugin.
LRESULT CALLBACK procedure(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_ERASEBKGND)
        return 1;

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

// Registered once under a name of its own, so two programs' copies of SymbolEngine in one process never share it.
const wchar_t* viewClass()
{
    static const std::wstring name = [] {
        HMODULE module = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&procedure), &module);
        auto className = L"SymbolEngineEditorView_" + std::to_wstring(reinterpret_cast<std::uintptr_t>(&procedure));
        WNDCLASSEXW view{};
        view.cbSize = sizeof view;
        view.lpfnWndProc = procedure;
        view.hInstance = module;
        view.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
        view.lpszClassName = className.c_str();
        RegisterClassExW(&view);
        return className;
    }();
    return name.c_str();
}

}

void prepare() {}

void* makeContainer(ui::Window& window)
{
    const auto parent = static_cast<HWND>(window.nativeHandle());
    RECT client{};
    GetClientRect(parent, &client);
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&procedure), &module);
    return CreateWindowExW(0, viewClass(), L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0,
                           client.right, client.bottom, parent, nullptr, module, nullptr);
}

void releaseContainer(void* container)
{
    DestroyWindow(static_cast<HWND>(container));
}

void settle(void*, std::weak_ptr<bool>) {}

Awareness::Awareness() : previous(SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {}

Awareness::~Awareness()
{
    if (previous != nullptr)
        SetThreadDpiAwarenessContext(static_cast<DPI_AWARENESS_CONTEXT>(previous));
}

}
