#include <symbolengine/app/Application.h>

#include <symbolengine/ui/MessageLoop.h>

#include <catch2/catch_test_macros.hpp>

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#else
    #include <spawn.h>
    #include <sys/wait.h>
    #include <unistd.h>
extern char** environ;
#endif

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace symbolengine;

namespace
{
// A program started with arguments, and waited for.
struct Program
{
#if defined(_WIN32)
    HANDLE process = nullptr;
#else
    pid_t process = -1;
#endif

    static Program start(const std::string& path, const std::vector<std::string>& arguments)
    {
        Program p;
#if defined(_WIN32)
        const auto wide = [](const std::string& text) {
            std::wstring result(static_cast<std::size_t>(
                                    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0)),
                                L'\0');
            MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(),
                                static_cast<int>(result.size()));
            return result;
        };
        // Each argument quoted, so one with a space stays one argument.
        std::wstring line = L"\"" + wide(path) + L"\"";
        for (const auto& argument : arguments)
            line += L" \"" + wide(argument) + L"\"";
        STARTUPINFOW startup{};
        startup.cb = sizeof startup;
        PROCESS_INFORMATION info{};
        if (CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &info))
        {
            CloseHandle(info.hThread);
            p.process = info.hProcess;
        }
#else
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(path.c_str()));
        for (const auto& argument : arguments)
            argv.push_back(const_cast<char*>(argument.c_str()));
        argv.push_back(nullptr);
        if (posix_spawn(&p.process, path.c_str(), nullptr, nullptr, argv.data(), environ) != 0)
            p.process = -1;
#endif
        return p;
    }

    // Its exit code, or -1 if it couldn't be started.
    int wait()
    {
#if defined(_WIN32)
        if (process == nullptr)
            return -1;
        WaitForSingleObject(process, 30000);
        DWORD code = static_cast<DWORD>(-1);
        GetExitCodeProcess(process, &code);
        CloseHandle(process);
        return static_cast<int>(code);
#else
        if (process < 0)
            return -1;
        int status = 0;
        waitpid(process, &status, 0);
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
    }
};

std::string processId()
{
#if defined(_WIN32)
    return std::to_string(GetCurrentProcessId());
#else
    return std::to_string(getpid());
#endif
}

// Runs the loop until the condition holds or the seconds pass, saying whether it held.
bool runUntil(Application& app, const std::function<bool()>& condition, int seconds = 15)
{
    bool held = condition();
    if (held)
        return true;
    bool timedOut = false;
    ui::messageLoop::callAfter(static_cast<std::uint32_t>(seconds * 1000), [&] {
        timedOut = true;
        app.quit();
    });
    // Looked at often, since what sets it may come from the system rather than from the program's own calls.
    std::function<void()> look = [&] {
        if (timedOut)
            return;
        held = condition();
        if (held)
            app.quit();
        else
            ui::messageLoop::callAfter(10, look);
    };
    look();
    app.run();
    return held;
}
}

TEST_CASE("An Application is the one the program made, with its arguments as UTF-8 and without the program's path")
{
    CHECK(Application::current() == nullptr);
    {
        const char* argv[] = {"program", "one", "zwei \xc3\xb6", "three four", nullptr};
        Application app({.name = "SymbolEngine app tests"}, 4, const_cast<char**>(argv));
        CHECK(Application::current() == &app);
        CHECK(app.options().name == "SymbolEngine app tests");
        CHECK(app.options().identifier == "SymbolEngine app tests");
        CHECK_FALSE(app.isQuitting());

        const auto& arguments = app.arguments();
#if defined(_WIN32)
        // On Windows they come from the process's own command line, which is the test runner's.
        CHECK(std::none_of(arguments.begin(), arguments.end(),
                           [](const std::string& a) { return a.size() > 4 && a.ends_with(".exe"); }));
#else
        CHECK(arguments == std::vector<std::string>{"one", "zwei \xc3\xb6", "three four"});
#endif
    }
    CHECK(Application::current() == nullptr);
}

TEST_CASE("run() returns the code given to quit(), from another thread too, and at once when quit() came first")
{
    {
        Application app({.name = "SymbolEngine app tests"}, 0, nullptr);
        std::thread other([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            app.quit(7);
        });
        CHECK(app.run() == 7);
        other.join();
        CHECK(app.isQuitting());
    }

    // Asked to quit with no handler, it quits, and run() has nothing left to do.
    {
        Application app({.name = "SymbolEngine app tests"}, 0, nullptr);
        app.requestQuit();
        CHECK(app.isQuitting());
        CHECK(app.run() == 0);
    }

    // With a handler, the handler decides.
    {
        int asked = 0;
        Application app({.name = "SymbolEngine app tests"}, 0, nullptr);
        app.onQuitRequested = [&] {
            ++asked;
            app.quit(3);
        };
        ui::messageLoop::callAsync([&] { app.requestQuit(); });
        CHECK(app.run() == 3);
        CHECK(asked == 1);
    }
}

TEST_CASE("A second instance hands its arguments to the one running, and runs alone when none is")
{
    const auto identifier = "dev.symbolengine.tests.instance." + processId();
    Application app({.name = "SymbolEngine app tests", .identifier = identifier}, 0, nullptr);
    REQUIRE_FALSE(app.sendToRunningInstance());   // this one is now the running instance
    REQUIRE_FALSE(app.sendToRunningInstance());   // and stays it

    std::vector<std::string> received;
    bool heard = false;
    app.onAnotherInstance = [&](std::vector<std::string> arguments) {
        received = std::move(arguments);
        heard = true;
    };

    // The second instance hands over while this one's loop runs, since on Windows it waits for the hand-over to be
    // taken.
    const std::vector<std::string> sent{identifier, "one", "zwei \xc3\xb6", "three four"};
    auto second = Program::start(SYMBOLENGINE_SECOND_INSTANCE, sent);
    CHECK(runUntil(app, [&] { return heard; }));
    CHECK(second.wait() == 0);
    CHECK(received == sent);

    // Under a name nobody runs under, the second instance finds no one and runs alone.
    CHECK(Program::start(SYMBOLENGINE_SECOND_INSTANCE, {identifier + ".nobody"}).wait() == 1);
}

TEST_CASE("A hand-over works whatever the length of the program's identifier")
{
    const auto identifier = "dev.symbolengine.tests." + std::string(150, 'x') + "." + processId();
    Application app({.name = "SymbolEngine app tests", .identifier = identifier}, 0, nullptr);
    REQUIRE_FALSE(app.sendToRunningInstance());

    bool heard = false;
    app.onAnotherInstance = [&](std::vector<std::string>) { heard = true; };
    auto second = Program::start(SYMBOLENGINE_SECOND_INSTANCE, {identifier, "one"});
    CHECK(runUntil(app, [&] { return heard; }));
    CHECK(second.wait() == 0);
}

#if defined(_WIN32)
TEST_CASE("When the session is ending for certain the program has a last word, as nothing runs after it")
{
    const auto identifier = "dev.symbolengine.tests.ended." + processId();
    Application app({.name = "SymbolEngine app tests", .identifier = identifier}, 0, nullptr);
    const std::wstring wide(identifier.begin(), identifier.end());
    HWND window = FindWindowW(L"SymbolEngineApplication", wide.c_str());
    REQUIRE(window != nullptr);

    int told = 0;
    app.onSessionEnding = [&] { ++told; };
    SendMessageW(window, WM_ENDSESSION, FALSE, 0);
    CHECK(told == 0);
    SendMessageW(window, WM_ENDSESSION, TRUE, 0);
    CHECK(told == 1);
}

TEST_CASE("On Windows the session ending asks the program to quit, and is told whether it is going to")
{
    const auto identifier = "dev.symbolengine.tests.session." + processId();
    Application app({.name = "SymbolEngine app tests", .identifier = identifier}, 0, nullptr);
    const std::wstring wide(identifier.begin(), identifier.end());
    HWND window = FindWindowW(L"SymbolEngineApplication", wide.c_str());
    REQUIRE(window != nullptr);

    int asked = 0;
    app.onQuitRequested = [&] { ++asked; };
    CHECK(SendMessageW(window, WM_QUERYENDSESSION, 0, 0) == FALSE);   // asked, and still deciding
    CHECK(asked == 1);

    app.onQuitRequested = [&] {
        ++asked;
        app.quit();
    };
    CHECK(SendMessageW(window, WM_QUERYENDSESSION, 0, 0) == TRUE);
    CHECK(asked == 2);
    CHECK(app.run() == 0);
}
#endif
