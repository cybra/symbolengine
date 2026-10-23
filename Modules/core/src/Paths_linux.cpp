#include "Platform.h"

#include "symbolengine/core/Files.h"
#include "symbolengine/core/Text.h"

#include <cstdlib>
#include <string>
#include <string_view>

namespace symbolengine::platform
{

namespace
{
// An XDG base folder: the variable when it holds an absolute path, as the specification requires, and otherwise
// its default under the home folder.
std::filesystem::path baseFolder(const char* variable, const char* underHome, std::error_code& error)
{
    if (const char* value = std::getenv(variable); value != nullptr && value[0] == '/')
        return value;

    auto home = posixHome(error);
    if (error)
        return {};
    return home / underHome;
}

// A folder named in user-dirs.dirs, such as XDG_MUSIC_DIR="$HOME/Music", or its usual name under the home folder.
std::filesystem::path userFolder(std::string_view key, const char* usualName, std::error_code& error)
{
    auto home = posixHome(error);
    if (error)
        return {};

    std::error_code ignored;
    const auto config = baseFolder("XDG_CONFIG_HOME", ".config", ignored);
    const auto lines = files::read(config / "user-dirs.dirs", ignored);

    for (auto line : text::split(lines, "\n"))
    {
        line = text::trim(line);
        if (!line.starts_with(key) || line.substr(key.size(), 1) != "=")
            continue;

        auto value = text::trim(line.substr(key.size() + 1));
        if (value.size() < 2 || value.front() != '"' || value.back() != '"')
            continue;
        value = value.substr(1, value.size() - 2);

        // The file is written in shell quoting, where a backslash keeps the next character as it is.
        std::string path;
        for (std::size_t i = 0; i < value.size(); ++i)
        {
            if (value[i] == '\\' && i + 1 < value.size())
                ++i;
            path += value[i];
        }

        constexpr std::string_view homeVariable = "$HOME";
        if (std::string_view(path).starts_with(homeVariable))
        {
            std::string_view rest(path);
            rest.remove_prefix(homeVariable.size());
            while (rest.starts_with('/'))
                rest.remove_prefix(1);
            return rest.empty() ? home : home / rest;
        }

        if (path.starts_with('/'))
            return path;
    }

    return home / usualName;
}
}

std::filesystem::path systemFolder(paths::Folder folder, std::error_code& error)
{
    using paths::Folder;

    switch (folder)
    {
        case Folder::home: return posixHome(error);
        case Folder::documents: return userFolder("XDG_DOCUMENTS_DIR", "Documents", error);
        case Folder::music: return userFolder("XDG_MUSIC_DIR", "Music", error);
        case Folder::desktop: return userFolder("XDG_DESKTOP_DIR", "Desktop", error);
        case Folder::downloads: return userFolder("XDG_DOWNLOAD_DIR", "Downloads", error);
        case Folder::appData: return baseFolder("XDG_CONFIG_HOME", ".config", error);
        case Folder::localAppData: return baseFolder("XDG_DATA_HOME", ".local/share", error);
        case Folder::cache: return baseFolder("XDG_CACHE_HOME", ".cache", error);
        case Folder::temp: break;
    }

    error = std::make_error_code(std::errc::invalid_argument);
    return {};
}

std::filesystem::path executableFile(std::error_code& error)
{
    return std::filesystem::read_symlink("/proc/self/exe", error);
}

}
