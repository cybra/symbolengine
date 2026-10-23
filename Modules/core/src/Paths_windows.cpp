#include "Platform.h"

#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
    #define NOMINMAX
#endif
#include <windows.h>

#include <knownfolders.h>
#include <shlobj.h>
#include <string>

namespace symbolengine::platform
{

namespace
{
std::filesystem::path knownFolder(REFKNOWNFOLDERID id, std::error_code& error)
{
    PWSTR found = nullptr;
    const HRESULT result = SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &found);

    if (FAILED(result))
    {
        CoTaskMemFree(found);
        error = std::error_code(static_cast<int>(result), std::system_category());
        return {};
    }

    std::filesystem::path path(found);
    CoTaskMemFree(found);
    return path;
}
}

std::filesystem::path systemFolder(paths::Folder folder, std::error_code& error)
{
    using paths::Folder;

    switch (folder)
    {
        case Folder::home: return knownFolder(FOLDERID_Profile, error);
        case Folder::documents: return knownFolder(FOLDERID_Documents, error);
        case Folder::music: return knownFolder(FOLDERID_Music, error);
        case Folder::desktop: return knownFolder(FOLDERID_Desktop, error);
        case Folder::downloads: return knownFolder(FOLDERID_Downloads, error);
        case Folder::appData: return knownFolder(FOLDERID_RoamingAppData, error);
        case Folder::localAppData:
        case Folder::cache: return knownFolder(FOLDERID_LocalAppData, error);
        case Folder::temp: break;
    }

    error = std::make_error_code(std::errc::invalid_argument);
    return {};
}

std::filesystem::path executableFile(std::error_code& error)
{
    std::wstring path(MAX_PATH, L'\0');

    for (;;)
    {
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));

        if (length == 0)
        {
            error = std::error_code(static_cast<int>(GetLastError()), std::system_category());
            return {};
        }

        if (length < path.size())
        {
            path.resize(length);
            return path;
        }

        path.resize(path.size() * 2);   // the path was cut short to fit
    }
}

}
