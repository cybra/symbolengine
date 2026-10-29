// The editor test plugin's view on Windows: a child window of its own in the host's, with another off screen beside
// it, registered for dropped files as plugins that take them are.

#include "TestEditor.h"

#include "pluginterfaces/gui/iplugview.h"

#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
    #define NOMINMAX
#endif
#include <windows.h>

#include <ole2.h>

#include <atomic>
#include <cstring>

namespace
{

constexpr RECT offscreenFrame{-1000, -1000, -990, -990};

// Takes nothing dropped on it: being registered is what the test asks.
class DropTarget final : public IDropTarget
{
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID wanted, void** object) override
    {
        if (wanted == IID_IUnknown || wanted == IID_IDropTarget)
        {
            *object = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }

        *object = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const auto left = --references;

        if (left == 0)
            delete this;

        return left;
    }

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject*, DWORD, POINTL, DWORD* effect) override
    {
        *effect = DROPEFFECT_NONE;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD* effect) override
    {
        *effect = DROPEFFECT_NONE;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragLeave() override { return S_OK; }

    HRESULT STDMETHODCALLTYPE Drop(IDataObject*, DWORD, POINTL, DWORD* effect) override
    {
        *effect = DROPEFFECT_NONE;
        return S_OK;
    }

private:
    std::atomic<ULONG> references{1};
};

struct Native
{
    HWND parent = nullptr;
    HWND own = nullptr;
    HWND offscreen = nullptr;
    HRESULT dropping = E_FAIL;
    DropTarget* target = nullptr;
};

HMODULE thisModule()
{
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&thisModule), &module);
    return module;
}

const wchar_t* viewClass()
{
    static const wchar_t* name = [] {
        WNDCLASSEXW view{};
        view.cbSize = sizeof view;
        view.lpfnWndProc = DefWindowProcW;
        view.hInstance = thisModule();
        view.lpszClassName = L"SymbolEngineTestEditorView";
        RegisterClassExW(&view);
        return L"SymbolEngineTestEditorView";
    }();
    return name;
}

// A child window's place in its parent.
RECT placeIn(HWND child)
{
    RECT frame{};
    GetWindowRect(child, &frame);
    MapWindowPoints(nullptr, GetParent(child), reinterpret_cast<POINT*>(&frame), 2);
    return frame;
}

}

namespace testEditor
{

bool supported(const char* platformType)
{
    return std::strcmp(platformType, Steinberg::kPlatformTypeHWND) == 0;
}

void* attach(void* parent, int width, int height)
{
    auto* native = new Native;
    native->parent = static_cast<HWND>(parent);
    native->own = CreateWindowExW(0, viewClass(), L"", WS_CHILD | WS_VISIBLE, 0, 0, width, height, native->parent,
                                  nullptr, thisModule(), nullptr);
    native->offscreen =
        CreateWindowExW(0, viewClass(), L"", WS_CHILD, offscreenFrame.left, offscreenFrame.top,
                        offscreenFrame.right - offscreenFrame.left, offscreenFrame.bottom - offscreenFrame.top,
                        native->parent, nullptr, thisModule(), nullptr);

    if (native->own == nullptr)
    {
        delete native;
        return nullptr;
    }

    native->target = new DropTarget;
    native->dropping = RegisterDragDrop(native->own, native->target);
    return native;
}

void resize(void* view, int width, int height)
{
    SetWindowPos(static_cast<Native*>(view)->own, nullptr, 0, 0, width, height,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void detach(void* view)
{
    auto* native = static_cast<Native*>(view);

    if (native->dropping == S_OK)
        RevokeDragDrop(native->own);

    native->target->Release();
    DestroyWindow(native->offscreen);
    DestroyWindow(native->own);
    delete native;
}

bool stillWhole(void* parent, void* view)
{
    auto* native = static_cast<Native*>(view);
    const auto host = static_cast<HWND>(parent);
    return host == native->parent && IsWindow(host) && IsWindow(native->own) && GetParent(native->own) == host &&
           GetParent(native->offscreen) == host;
}

// A plain child window that clips its children and siblings, made per-monitor aware.
bool laidOut(void* parent)
{
    const auto host = static_cast<HWND>(parent);
    const auto styles = static_cast<DWORD>(GetWindowLongPtrW(host, GWL_STYLE));
    const DWORD wanted = WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
    return IsWindow(host) && (styles & wanted) == wanted &&
           AreDpiAwarenessContextsEqual(GetWindowDpiAwarenessContext(host), DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
}

bool sized(void* parent, int width, int height)
{
    RECT client{};
    GetClientRect(static_cast<HWND>(parent), &client);
    return client.right == width && client.bottom == height;
}

void ownSize(void* view, int& width, int& height)
{
    RECT frame{};
    GetWindowRect(static_cast<Native*>(view)->own, &frame);
    width = frame.right - frame.left;
    height = frame.bottom - frame.top;
}

bool awareAsParent(void* parent)
{
    return AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(),
                                        GetWindowDpiAwarenessContext(static_cast<HWND>(parent)));
}

bool takesDrops(void* view)
{
    return static_cast<Native*>(view)->dropping == S_OK;
}

bool offscreenMoved(void* view)
{
    const auto now = placeIn(static_cast<Native*>(view)->offscreen);
    return !EqualRect(&now, &offscreenFrame);
}

int nudges(void*)
{
    return 0;
}

}
