#include "symbolengine/core/Paths.h"

#include "Platform.h"

#if !defined(_WIN32)
    #include <cstdlib>
    #include <pwd.h>
    #include <unistd.h>
    #include <vector>
#endif

namespace symbolengine::paths
{

std::filesystem::path fromUtf8(std::string_view text)
{
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

std::string toUtf8(const std::filesystem::path& path)
{
    const auto text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
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
