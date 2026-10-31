#include "Platform.h"

#include <symbolengine/ui/MessageLoop.h>

#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
    #define NOMINMAX
#endif
#include <windows.h>

#include <shellapi.h>
#include <shlobj.h>

#include <chrono>
#include <cstdio>
#include <io.h>
#include <thread>

namespace symbolengine::app::platform
{

namespace
{
constexpr wchar_t windowClass[] = L"SymbolEngineApplication";
constexpr ULONG_PTR argumentsHandedOver = 0x53594D42;   // marks a WM_COPYDATA carrying another instance's arguments

HWND window = nullptr;        // hidden; the system and other instances reach the program through it
HANDLE instanceMutex = nullptr;   // held by the running instance

std::wstring wide(const std::string& text)
{
    if (text.empty())
        return {};
    const auto length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length);
    return result;
}

std::string utf8(std::wstring_view text)
{
    if (text.empty())
        return {};
    const auto length =
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length, nullptr, nullptr);
    return result;
}

// The arguments as another instance sends them: each ended by a zero, one after another.
std::vector<std::string> argumentsIn(const COPYDATASTRUCT& data)
{
    std::vector<std::string> arguments;
    const auto* text = static_cast<const wchar_t*>(data.lpData);
    const auto count = data.cbData / sizeof(wchar_t);
    std::size_t start = 0;
    for (std::size_t i = 0; i < count; ++i)
        if (text[i] == L'\0')
        {
            arguments.push_back(utf8(std::wstring_view(text + start, i - start)));
            start = i + 1;
        }
    return arguments;
}

LRESULT CALLBACK windowProcedure(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
        case WM_COPYDATA:
        {
            const auto* data = reinterpret_cast<const COPYDATASTRUCT*>(lParam);
            if (data == nullptr || data->dwData != argumentsHandedOver)
                break;
            // Read before returning, since the sender's copy goes when this returns; acted on once the sender is
            // free again.
            auto arguments = argumentsIn(*data);
            ui::messageLoop::callAsync([arguments = std::move(arguments)]() mutable {
                if (auto* app = Application::current(); app != nullptr && app->onAnotherInstance)
                    app->onAnotherInstance(std::move(arguments));
            });
            return TRUE;
        }

        // The session is ending: the program is asked to quit, and the system is told whether it is going to.
        case WM_QUERYENDSESSION:
        {
            auto* app = Application::current();
            if (app == nullptr)
                return TRUE;
            app->requestQuit();
            return app->isQuitting() ? TRUE : FALSE;
        }

        // It really is ending, and the process goes when this returns, whatever it has yet to do.
        case WM_ENDSESSION:
        {
            if (wParam != FALSE)
                if (auto* app = Application::current(); app != nullptr && app->onSessionEnding)
                    app->onSessionEnding();
            return 0;
        }

        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

// A program started from a console prints to it, as a console program would, unless its output was already sent
// somewhere, as by a redirect.
void joinParentConsole()
{
    if (AttachConsole(ATTACH_PARENT_PROCESS) == 0)
        return;
    FILE* ignored = nullptr;
    if (_fileno(stdout) < 0)
        freopen_s(&ignored, "CONOUT$", "w", stdout);
    if (_fileno(stderr) < 0)
        freopen_s(&ignored, "CONOUT$", "w", stderr);
    if (_fileno(stdin) < 0)
        freopen_s(&ignored, "CONIN$", "r", stdin);
}

// Each window is drawn at its own screen's scaling, and told when that changes, as Windows 10 from 1703 allows; an
// older Windows is asked for the one scaling it has.
void beAwareOfScaling()
{
    using SetContext = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll"))
        if (auto found = GetProcAddress(user32, "SetProcessDpiAwarenessContext"))
        {
            reinterpret_cast<SetContext>(reinterpret_cast<void*>(found))(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
            return;
        }
    SetProcessDPIAware();
}

// Another instance's window, by its class and the identifier, passing over this instance's own.
HWND findRunningInstance(const std::wstring& identifier)
{
    HWND found = nullptr;
    while ((found = FindWindowExW(nullptr, found, windowClass, identifier.c_str())) != nullptr)
        if (found != window)
            return found;
    return nullptr;
}
}

void start(Application&, const std::string& identifier)
{
    beAwareOfScaling();
    joinParentConsole();

    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&windowProcedure), &module);

    WNDCLASSEXW c{};
    c.cbSize = sizeof c;
    c.lpfnWndProc = windowProcedure;
    c.hInstance = module;
    c.lpszClassName = windowClass;
    RegisterClassExW(&c);

    // A window of its own at the top, never shown: the session's end reaches those, where a message-only window
    // hears nothing of it.
    window = CreateWindowExW(0, windowClass, wide(identifier).c_str(), WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr,
                             module, nullptr);
}

void finish()
{
    if (window != nullptr)
    {
        DestroyWindow(window);
        window = nullptr;
    }
    if (instanceMutex != nullptr)
    {
        CloseHandle(instanceMutex);
        instanceMutex = nullptr;
    }
}

std::vector<std::string> arguments(int, char**)
{
    std::vector<std::string> given;
    int count = 0;
    if (wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &count))
    {
        for (int i = 1; i < count; ++i)
            given.push_back(utf8(argv[i]));
        LocalFree(argv);
    }
    return given;
}

// The running instance holds a named mutex, which the system lets go of however the process ends, and hears from
// later instances through its hidden window, found by its class and the identifier.
bool sendToRunningInstance(const std::string& identifier, const std::vector<std::string>& arguments)
{
    if (instanceMutex != nullptr)
        return false;

    const auto name = wide(identifier);
    HANDLE mutex = CreateMutexW(nullptr, FALSE, (L"Local\\" + name + L".symbolengine.instance").c_str());
    if (mutex == nullptr)
        return false;

    if (GetLastError() != ERROR_ALREADY_EXISTS)
    {
        instanceMutex = mutex;
        return false;
    }
    CloseHandle(mutex);

    // The running instance may still be starting up, its window not yet made.
    HWND running = nullptr;
    for (int tries = 0; running == nullptr && tries < 60; ++tries)
    {
        running = findRunningInstance(name);
        if (running == nullptr)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (running == nullptr)
        return false;

    std::wstring text;
    for (const auto& argument : arguments)
    {
        text += wide(argument);
        text += L'\0';
    }
    COPYDATASTRUCT data{};
    data.dwData = argumentsHandedOver;
    data.cbData = static_cast<DWORD>(text.size() * sizeof(wchar_t));
    data.lpData = text.empty() ? nullptr : text.data();

    // Its window can come forward, which only a program in front can allow.
    DWORD process = 0;
    GetWindowThreadProcessId(running, &process);
    AllowSetForegroundWindow(process);

    // Only an answer means it has taken them, and that this one can go.
    DWORD_PTR result = 0;
    return SendMessageTimeoutW(running, WM_COPYDATA, reinterpret_cast<WPARAM>(window), reinterpret_cast<LPARAM>(&data),
                               SMTO_ABORTIFHUNG | SMTO_BLOCK, 5000, &result) != 0 &&
           result == TRUE;
}

bool isForeground()
{
    DWORD process = 0;
    if (HWND front = GetForegroundWindow())
        GetWindowThreadProcessId(front, &process);
    return process == GetCurrentProcessId();
}

void setDockIconVisible(bool)
{
}

void noteRecentFile(const std::filesystem::path& file)
{
    SHAddToRecentDocs(SHARD_PATHW, file.c_str());
}

void declineQuit() {}

void terminate(int code)
{
    ExitProcess(static_cast<UINT>(code));
}

}
