#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

// What a scan reads of a plugin's files without loading any code.
namespace symbolengine::pluginFiles
{

/** The architectures a binary is built for, read from its headers, whichever system's format it is in: a Mac's
    Mach-O, thin or universal, Windows' PE or Linux's ELF; none for a file that is none of them. */
std::vector<std::string> architecturesOf(const std::filesystem::path& binary);

/** A file's modification time in nanoseconds since 1970 on the system clock, or 0 with the error set. */
std::int64_t modifiedNanoseconds(const std::filesystem::path& file, std::error_code& error);

/** A time on the file system's clock in nanoseconds since 1970 on the system clock. */
std::int64_t nanosecondsOf(std::filesystem::file_time_type time);

/** The version a binary or bundle declares to its system, read without loading it: a Mac bundle's Info.plist, its
    CFBundleShortVersionString and CFBundleVersion; a Windows binary's version resource; nothing on Linux. Empty where
    it declares none. */
std::string declaredVersion(const std::filesystem::path& bundle, const std::filesystem::path& binary);

}
