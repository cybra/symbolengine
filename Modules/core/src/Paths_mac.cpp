#include "Platform.h"

#include <climits>
#include <cstdint>
#include <cstring>
#include <mach-o/dyld.h>
#include <string>
#include <string_view>
#include <sysdir.h>

namespace symbolengine::platform
{

namespace
{
std::filesystem::path userFolder(sysdir_search_path_directory_t which, std::error_code& error)
{
    char found[PATH_MAX] = {};
    auto state = sysdir_start_search_path_enumeration(which, SYSDIR_DOMAIN_MASK_USER);
    state = sysdir_get_next_search_path_enumeration(state, found);

    if (state == 0)
    {
        error = std::make_error_code(std::errc::no_such_file_or_directory);
        return {};
    }

    // The user's folders come back starting with a tilde for the home folder.
    std::string_view path(found);
    if (!path.starts_with('~'))
        return paths::fromUtf8(path);

    auto home = posixHome(error);
    if (error)
        return {};

    path.remove_prefix(1);
    while (path.starts_with('/'))
        path.remove_prefix(1);
    return home / paths::fromUtf8(path);
}
}

std::filesystem::path systemFolder(paths::Folder folder, std::error_code& error)
{
    using paths::Folder;

    switch (folder)
    {
        case Folder::home: return posixHome(error);
        case Folder::documents: return userFolder(SYSDIR_DIRECTORY_DOCUMENT, error);
        case Folder::music: return userFolder(SYSDIR_DIRECTORY_MUSIC, error);
        case Folder::desktop: return userFolder(SYSDIR_DIRECTORY_DESKTOP, error);
        case Folder::downloads: return userFolder(SYSDIR_DIRECTORY_DOWNLOADS, error);
        case Folder::appData:
        case Folder::localAppData: return userFolder(SYSDIR_DIRECTORY_APPLICATION_SUPPORT, error);
        case Folder::cache: return userFolder(SYSDIR_DIRECTORY_CACHES, error);
        case Folder::temp: break;
    }

    error = std::make_error_code(std::errc::invalid_argument);
    return {};
}

std::filesystem::path executableFile(std::error_code& error)
{
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);

    std::string path(size, '\0');
    if (_NSGetExecutablePath(path.data(), &size) != 0)
    {
        error = std::make_error_code(std::errc::no_such_file_or_directory);
        return {};
    }

    path.resize(std::strlen(path.c_str()));
    return std::filesystem::weakly_canonical(paths::fromUtf8(path), error);
}

}
