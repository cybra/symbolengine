#include "symbolengine/core/System.h"

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>

    // After windows.h, which it needs.
    #include <shellapi.h>

    #include <filesystem>
    #include <vector>
#else
    #include <cstdlib>
    #include <spawn.h>
    #include <sys/wait.h>
    #if defined(__APPLE__)
        #include <crt_externs.h>
        #include <sys/sysctl.h>
        #include <sys/time.h>
    #else
        #include <ctime>
        #include <fstream>
        #include <unistd.h>
extern char** environ;
    #endif
#endif

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace symbolengine::system
{

std::optional<std::string> environmentVariable(std::string_view name)
{
#if defined(_WIN32)
    // Windows keeps the environment in UTF-16; std::getenv would give it in the system's code page.
    const auto wide = std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(name.data()), name.size()));
    const auto needed = GetEnvironmentVariableW(wide.c_str(), nullptr, 0);
    if (needed == 0)
        return std::nullopt;

    std::vector<wchar_t> value(needed);
    const auto length = GetEnvironmentVariableW(wide.c_str(), value.data(), needed);
    const auto text = std::filesystem::path(std::wstring(value.data(), length)).u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
#else
    if (const char* value = std::getenv(std::string(name).c_str()))
        return std::string(value);
    return std::nullopt;
#endif
}

bool openInBrowser(std::string_view url)
{
    const auto starts = [&url](std::string_view scheme) {
        return url.size() > scheme.size() && std::equal(scheme.begin(), scheme.end(), url.begin(), [](char a, char b) {
                   return a == static_cast<char>(std::tolower(static_cast<unsigned char>(b)));
               });
    };
    const bool web = starts("http://") || starts("https://");
    const bool plain = std::none_of(
        url.begin(), url.end(), [](char c) { return static_cast<unsigned char>(c) < 0x21 || c == '"' || c == '\\'; });
    if (!web || !plain)
        return false;

#if defined(_WIN32)
    const auto wide = std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(url.data()), url.size()));
    const auto result =
        reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    return result > 32;
#else
    // The system's own opener, given the address as one argument, with no shell between.
    #if defined(__APPLE__)
    const char* opener = "/usr/bin/open";
    #else
    const char* opener = "xdg-open";
    #endif
    std::string address(url);
    char* const arguments[] = {const_cast<char*>(opener), address.data(), nullptr};
    pid_t child = 0;
    #if defined(__APPLE__)
    char** const environment = *_NSGetEnviron();   // environ itself isn't there for a plugin's bundle
    #else
    char** const environment = environ;
    #endif
    if (posix_spawnp(&child, opener, nullptr, nullptr, arguments, environment) != 0)
        return false;
    int status = 0;
    return waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
}

std::string_view architecture()
{
#if defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#elif defined(__arm__) || defined(_M_ARM)
    return "arm";
#elif defined(__i386__) || defined(_M_IX86)
    return "x86";
#else
    return "unknown";
#endif
}

std::chrono::system_clock::time_point computerStarted()
{
    using namespace std::chrono;
#if defined(_WIN32)
    // The time since it started counts its sleep too.
    return system_clock::now() - duration_cast<system_clock::duration>(milliseconds(GetTickCount64()));
#elif defined(__APPLE__)
    timeval started{};
    std::size_t size = sizeof started;
    int name[] = {CTL_KERN, KERN_BOOTTIME};
    if (sysctl(name, 2, &started, &size, nullptr, 0) == 0)
        return system_clock::time_point(
            duration_cast<system_clock::duration>(seconds(started.tv_sec) + microseconds(started.tv_usec)));
    return {};
#else
    // The kernel's own record of when it started, in whole seconds.
    std::ifstream stat("/proc/stat");
    for (std::string line; std::getline(stat, line);)
        if (line.starts_with("btime "))
            return system_clock::time_point(duration_cast<system_clock::duration>(seconds(std::stoll(line.substr(6)))));
    timespec since{};
    clock_gettime(CLOCK_BOOTTIME, &since);
    return system_clock::now() -
           duration_cast<system_clock::duration>(seconds(since.tv_sec) + nanoseconds(since.tv_nsec));
#endif
}

void endNow(int code)
{
#if defined(_WIN32)
    // ExitProcess would run every library's detach code with the other threads stopped where they stood.
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(code));
#endif
    std::_Exit(code);
}

}
