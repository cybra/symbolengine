#include "PluginWindowsNative.h"

#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
    #define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <cwchar>
#include <deque>
#include <string>

namespace symbolengine::pluginWindowsNative
{

namespace
{

constexpr UINT runPosted = WM_APP + 0x5E10;
constexpr UINT_PTR lookTimer = 1, beatTimer = 2;

// A key as an editor is offered it: the named keys by their virtual keys, and its character as the keyboard's layout
// gives it, with the modifier keys held.
PluginKey keyOf(WPARAM virtualKey, LPARAM flags)
{
    using Named = PluginKey::Named;
    PluginKey key;
    key.shift = GetKeyState(VK_SHIFT) < 0;
    key.alt = GetKeyState(VK_MENU) < 0;
    key.command = GetKeyState(VK_CONTROL) < 0;

    switch (virtualKey)
    {
        case VK_BACK: key.named = Named::backspace; break;
        case VK_TAB: key.named = Named::tab; break;
        case VK_RETURN: key.named = Named::enter; break;
        case VK_ESCAPE: key.named = Named::escape; break;
        case VK_SPACE: key.named = Named::space; break;
        case VK_LEFT: key.named = Named::left; break;
        case VK_UP: key.named = Named::up; break;
        case VK_RIGHT: key.named = Named::right; break;
        case VK_DOWN: key.named = Named::down; break;
        case VK_HOME: key.named = Named::home; break;
        case VK_END: key.named = Named::end; break;
        case VK_PRIOR: key.named = Named::pageUp; break;
        case VK_NEXT: key.named = Named::pageDown; break;
        case VK_INSERT: key.named = Named::insert; break;
        case VK_DELETE: key.named = Named::forwardDelete; break;
        default:
            if (virtualKey >= VK_F1 && virtualKey <= VK_F12)
                key.named = static_cast<Named>(static_cast<int>(Named::f1) + static_cast<int>(virtualKey - VK_F1));
            break;
    }

    // Read without changing the keyboard's state, as a dead key waiting for its letter would be.
    BYTE state[256] = {};
    GetKeyboardState(state);
    wchar_t text[4] = {};
    const auto scan = static_cast<UINT>((flags >> 16) & 0xFF);
    if (ToUnicode(static_cast<UINT>(virtualKey), scan, state, text, 4, 4) == 1)
        key.character = static_cast<char32_t>(text[0]);
    else if (virtualKey == VK_SPACE)
        key.character = U' ';

    return key;
}

}

struct Watch::Native
{
    Calls calls;
    HWND window = nullptr;
    HHOOK keys = nullptr;
    HWINEVENTHOOK shows = nullptr, fronts = nullptr;
    std::deque<std::function<void()>> posted;

    // The hooks are given no context of their own, and there is one watch at a time on the main thread.
    static inline Native* active = nullptr;

    static LRESULT CALLBACK procedure(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (active != nullptr && hwnd == active->window)
        {
            if (message == runPosted)
            {
                auto ready = std::move(active->posted);
                active->posted.clear();
                for (auto& function : ready)
                    function();
                return 0;
            }

            if (message == WM_TIMER && wParam == lookTimer)
            {
                active->calls.look();
                return 0;
            }

            if (message == WM_TIMER && wParam == beatTimer)
            {
                active->calls.beat();
                return 0;
            }
        }

        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    // A window of the process showing, hiding or going, or another coming in front: looked at again once it is over.
    static void CALLBACK windowEvent(HWINEVENTHOOK, DWORD, HWND hwnd, LONG object, LONG child, DWORD, DWORD)
    {
        if (active != nullptr && object == OBJID_WINDOW && child == CHILDID_SELF &&
            (hwnd == nullptr || GetAncestor(hwnd, GA_ROOT) == hwnd))
            PostMessageW(active->window, WM_TIMER, lookTimer, 0);
    }

    // Each key the main thread takes from its queue for a window, before it is translated and sent there: the program's
    // from a plugin window taken, unless text is being typed there; any other offered to the plugin's editor first and
    // sent to its window only if the editor doesn't take it, so no key arrives twice.
    static LRESULT CALLBACK message(int code, WPARAM removing, LPARAM lParam)
    {
        auto* const sent = reinterpret_cast<MSG*>(lParam);

        if (code == HC_ACTION && removing == PM_REMOVE && active != nullptr && sent != nullptr &&
            (sent->message == WM_KEYDOWN || sent->message == WM_KEYUP || sent->message == WM_SYSKEYDOWN ||
             sent->message == WM_SYSKEYUP))
        {
            const auto root = GetAncestor(sent->hwnd, GA_ROOT);
            const bool down = sent->message == WM_KEYDOWN || sent->message == WM_SYSKEYDOWN;
            const bool repeat = down && (sent->lParam & (LPARAM(1) << 30)) != 0;
            const auto key = keyOf(sent->wParam, sent->lParam);
            const bool plain = !key.shift && !key.alt && !key.command;

            if (root != nullptr && key.character != 0 && (!down || (plain && !typingIn(root))) &&
                active->calls.take(root, key.character, down, repeat))
                sent->message = WM_NULL;
            else if (root != nullptr && active->calls.offer(root, key, down))
                sent->message = WM_NULL;
        }

        return CallNextHookEx(nullptr, code, removing, lParam);
    }
};

Watch::Watch(Calls calls) : native(std::make_unique<Native>())
{
    native->calls = std::move(calls);
    Native::active = native.get();

    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&Native::procedure), &module);
    static const std::wstring className = [module] {
        auto name = L"SymbolEngineWindowWatch_" + std::to_wstring(reinterpret_cast<std::uintptr_t>(&Native::procedure));
        WNDCLASSEXW watch{};
        watch.cbSize = sizeof watch;
        watch.lpfnWndProc = &Native::procedure;
        watch.hInstance = module;
        watch.lpszClassName = name.c_str();
        RegisterClassExW(&watch);
        return name;
    }();

    // A window for messages alone, never shown: what is posted runs there, and the quarter-second look.
    native->window = CreateWindowExW(0, className.c_str(), L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, module, nullptr);
    SetTimer(native->window, lookTimer, 250, nullptr);
    SetTimer(native->window, beatTimer, beatMilliseconds, nullptr);
    native->keys = SetWindowsHookExW(WH_GETMESSAGE, &Native::message, nullptr, GetCurrentThreadId());
    native->shows = SetWinEventHook(EVENT_OBJECT_DESTROY, EVENT_OBJECT_HIDE, nullptr, &Native::windowEvent,
                                    GetCurrentProcessId(), 0, WINEVENT_OUTOFCONTEXT);
    native->fronts = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, &Native::windowEvent, 0,
                                     0, WINEVENT_OUTOFCONTEXT);
}

Watch::~Watch()
{
    if (native->fronts != nullptr)
        UnhookWinEvent(native->fronts);
    if (native->shows != nullptr)
        UnhookWinEvent(native->shows);
    if (native->keys != nullptr)
        UnhookWindowsHookEx(native->keys);
    KillTimer(native->window, lookTimer);
    KillTimer(native->window, beatTimer);
    DestroyWindow(native->window);

    if (Native::active == native.get())
        Native::active = nullptr;
}

void Watch::post(std::function<void()> function)
{
    native->posted.push_back(std::move(function));
    PostMessageW(native->window, runPosted, 0, 0);
}

bool inFront()
{
    DWORD process = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &process);
    return process == GetCurrentProcessId();
}

namespace
{

struct Search
{
    const std::function<bool(void*)>* known = nullptr;
    bool found = false;
};

// A visible window of the main thread's that isn't known, a tool window, a tooltip, a menu or a window of an input
// method's.
BOOL CALLBACK look(HWND window, LPARAM context)
{
    auto& search = *reinterpret_cast<Search*>(context);
    wchar_t name[64] = {};
    GetClassNameW(window, name, 64);

    if (!IsWindowVisible(window) || (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) != 0 ||
        std::wcscmp(name, L"tooltips_class32") == 0 || std::wcscmp(name, L"#32768") == 0 ||
        std::wcsncmp(name, L"IME", 3) == 0 || std::wcscmp(name, L"MSCTFIME UI") == 0 || (*search.known)(window))
        return TRUE;

    search.found = true;
    return FALSE;
}

}

bool otherShowing(const std::function<bool(void* window)>& known)
{
    Search search{&known, false};
    EnumThreadWindows(GetCurrentThreadId(), &look, reinterpret_cast<LPARAM>(&search));
    return search.found;
}

bool hasKeyboard(void* window)
{
    return GetForegroundWindow() == static_cast<HWND>(window);
}

bool typingIn(void* window)
{
    const auto focus = GetFocus();

    if (focus == nullptr || (focus != window && !IsChild(static_cast<HWND>(window), focus)))
        return false;

    wchar_t name[64] = {};
    GetClassNameW(focus, name, 64);
    return _wcsicmp(name, L"Edit") == 0 || _wcsnicmp(name, L"RichEdit", 8) == 0;
}

}
