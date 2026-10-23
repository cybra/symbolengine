#pragma once

#include "symbolengine/core/Paths.h"

#include <filesystem>
#include <system_error>

// What each system's own source file provides.
namespace symbolengine::platform
{

/** Any folder but temp, which every system finds the same way. */
std::filesystem::path systemFolder(paths::Folder folder, std::error_code& error);

std::filesystem::path executableFile(std::error_code& error);

#if !defined(_WIN32)
/** The home folder on a Mac or Linux: HOME, or the user's entry in the password database when HOME isn't set. */
std::filesystem::path posixHome(std::error_code& error);
#endif

}
