#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

/** Reading and writing whole files. Each function throws std::filesystem::filesystem_error, or, given a
    std::error_code, sets it instead, as std::filesystem's own functions do. */
namespace symbolengine::files
{

/** The file's contents, byte for byte. */
std::string read(const std::filesystem::path& file);
std::string read(const std::filesystem::path& file, std::error_code& error);

/** Writes the contents so that the file holds either what it held before or the new contents whole, even if the
    program or the computer stops partway: they are written to a new file beside it, flushed to the disk, and then put
    in its place in one step. Missing folders on the way are made. A file that already exists keeps its permissions,
    and a symbolic link is followed, so the file it points to is the one replaced. On Windows a file another program
    has open for a moment, as virus scanners and search indexers do, is waited for briefly. */
void writeSafely(const std::filesystem::path& file, std::string_view contents);
void writeSafely(const std::filesystem::path& file, std::string_view contents, std::error_code& error);

}
