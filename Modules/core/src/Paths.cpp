#include "symbolengine/core/Paths.h"

#include "Platform.h"

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>

    #include <string>
#else
    #include <cstdlib>
    #include <pwd.h>
    #include <unistd.h>
    #include <vector>
#endif

namespace symbolengine::paths
{

// On Windows, by the system's own conversions, which make a replacement of what isn't valid, where the standard library's
// throw: a name on disk can hold half a surrogate pair, which one bad file shouldn't let end a listing of them.
std::filesystem::path fromUtf8(std::string_view text)
{
#if defined(_WIN32)
    const auto size = static_cast<int>(text.size());
    std::wstring wide(
        static_cast<std::size_t>(size > 0 ? MultiByteToWideChar(CP_UTF8, 0, text.data(), size, nullptr, 0) : 0), L'\0');
    if (!wide.empty())
        MultiByteToWideChar(CP_UTF8, 0, text.data(), size, wide.data(), static_cast<int>(wide.size()));
    return std::filesystem::path(std::move(wide));
#else
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
#endif
}

std::string toUtf8(const std::filesystem::path& path)
{
#if defined(_WIN32)
    const auto& wide = path.native();
    const auto size = static_cast<int>(wide.size());
    std::string text(
        static_cast<std::size_t>(
            size > 0 ? WideCharToMultiByte(CP_UTF8, 0, wide.data(), size, nullptr, 0, nullptr, nullptr) : 0),
        '\0');
    if (!text.empty())
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), size, text.data(), static_cast<int>(text.size()), nullptr,
                            nullptr);
    return text;
#else
    const auto text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
#endif
}

std::filesystem::path systemFolder(Folder folder, std::error_code& error)
{
    error.clear();
    if (folder == Folder::temp)
        return std::filesystem::temp_directory_path(error);
    return platform::systemFolder(folder, error);
}

std::filesystem::path systemFolder(Folder folder)
{
    std::error_code error;
    auto path = systemFolder(folder, error);
    if (error)
        throw std::filesystem::filesystem_error("the system didn't say where this folder is", error);
    return path;
}

std::filesystem::path executableFile(std::error_code& error)
{
    error.clear();
    return platform::executableFile(error);
}

std::filesystem::path executableFile()
{
    std::error_code error;
    auto path = executableFile(error);
    if (error)
        throw std::filesystem::filesystem_error("the system didn't say where the program is", error);
    return path;
}

}

#if !defined(_WIN32)
namespace symbolengine::platform
{

std::filesystem::path posixHome(std::error_code& error)
{
    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] == '/')
        return home;

    std::vector<char> buffer(16384);
    passwd entry{};
    passwd* found = nullptr;

    if (getpwuid_r(getuid(), &entry, buffer.data(), buffer.size(), &found) == 0 && found != nullptr &&
        found->pw_dir != nullptr && found->pw_dir[0] == '/')
        return found->pw_dir;

    error = std::make_error_code(std::errc::no_such_file_or_directory);
    return {};
}

}
#endif
