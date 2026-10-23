#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

/** Paths as UTF-8 text, and the folders each system keeps a user's files in. */
namespace symbolengine::paths
{

/** A path from UTF-8 text. On Windows, std::filesystem::path's constructor from std::string reads the text in the
    system's code page instead, which garbles any name outside it. */
std::filesystem::path fromUtf8(std::string_view text);

/** A path as UTF-8 text. On Windows, path::string() writes the system's code page instead. */
std::string toUtf8(const std::filesystem::path& path);

/** The folders a system gives each user. */
enum class Folder
{
    home,
    documents,
    music,
    desktop,
    downloads,
    appData,        // settings and data that follow the user: ~/Library/Application Support on a Mac,
                    // AppData\Roaming on Windows, ~/.config on Linux
    localAppData,   // data kept on this computer: ~/Library/Application Support on a Mac, AppData\Local on Windows,
                    // ~/.local/share on Linux
    cache,          // files that can be made again: ~/Library/Caches on a Mac, AppData\Local on Windows, ~/.cache on
                    // Linux
    temp            // files for the moment
};

/** Where the system keeps that folder for the user running the program. The folder may not exist yet, apart from home
    and temp. On Linux the XDG variables and user-dirs.dirs are followed, as the XDG specifications describe. Throws
    std::filesystem::filesystem_error if the system won't say. */
std::filesystem::path systemFolder(Folder folder);
std::filesystem::path systemFolder(Folder folder, std::error_code& error);

/** The program's own executable file. */
std::filesystem::path executableFile();
std::filesystem::path executableFile(std::error_code& error);

}
