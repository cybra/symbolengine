#include "symbolengine/core/Files.h"

#include <atomic>
#include <cstdint>
#include <random>
#include <string>

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>

    #include <algorithm>
    #include <chrono>
    #include <thread>
#else
    #include <cerrno>
    #include <fcntl.h>
    #include <sys/stat.h>
    #include <unistd.h>
    #if defined(__APPLE__)
        #include <copyfile.h>
    #elif defined(__linux__)
        #include <sys/xattr.h>
    #endif
#endif

namespace symbolengine::files
{

namespace
{
// A name for the new file beside the one being replaced, never the same twice.
std::filesystem::path temporaryBeside(const std::filesystem::path& file)
{
    static std::atomic<std::uint64_t> count{0};
    thread_local std::mt19937_64 random{std::random_device{}()};

    constexpr char hex[] = "0123456789abcdef";
    std::uint64_t mix = random() ^ count.fetch_add(1);
    std::string tag;
    for (int i = 0; i < 12; ++i, mix >>= 4)
        tag += hex[mix & 0xF];

    // Its own name isn't part of this, which would make a name as long as the system allows too long.
    return file.parent_path() / std::filesystem::path("." + tag + ".tmp");
}

// Follows symbolic links to the file they lead to, so writing replaces that file and the links stay as they are.
std::filesystem::path followLinks(std::filesystem::path file, std::error_code& error)
{
    for (int hops = 0; hops < 40; ++hops)
    {
        std::error_code ignored;
        if (!std::filesystem::is_symlink(file, ignored))
            return file;

        auto target = std::filesystem::read_symlink(file, error);
        if (error)
            return {};
        file = target.is_absolute() ? target : file.parent_path() / target;
    }

    error = std::make_error_code(std::errc::too_many_symbolic_link_levels);
    return {};
}

#if defined(_WIN32)
std::error_code lastError()
{
    return std::error_code(static_cast<int>(GetLastError()), std::system_category());
}

void writeNew(const std::filesystem::path& file, std::string_view contents, std::error_code& error)
{
    HANDLE handle = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        error = lastError();
        return;
    }

    while (!contents.empty())
    {
        const auto chunk = static_cast<DWORD>(std::min<std::size_t>(contents.size(), 1u << 30));
        DWORD written = 0;
        if (!WriteFile(handle, contents.data(), chunk, &written, nullptr))
        {
            error = lastError();
            break;
        }
        contents.remove_prefix(written);
    }

    if (!error && !FlushFileBuffers(handle))
        error = lastError();

    CloseHandle(handle);
}

void replace(const std::filesystem::path& from, const std::filesystem::path& to, std::error_code& error)
{
    // Virus scanners, search indexers and backup tools open files for a moment after they change, and a file
    // open without delete sharing can't be replaced until they let go, so a refusal is tried again for a while.
    for (int attempt = 0;; ++attempt)
    {
        // What the system holds about a file it has, its attributes, permissions and when it was made, stay with it.
        if (ReplaceFileW(to.c_str(), from.c_str(), nullptr,
                         REPLACEFILE_IGNORE_MERGE_ERRORS | REPLACEFILE_IGNORE_ACL_ERRORS, nullptr, nullptr))
            return;

        if (GetLastError() == ERROR_FILE_NOT_FOUND &&
            MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return;

        const DWORD code = GetLastError();
        const bool busy =
            code == ERROR_ACCESS_DENIED || code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION;
        if (!busy || attempt >= 40)
        {
            error = std::error_code(static_cast<int>(code), std::system_category());
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
}

void removeQuietly(const std::filesystem::path& file)
{
    DeleteFileW(file.c_str());
}
#else
std::error_code lastError()
{
    return std::error_code(errno, std::generic_category());
}

// What the system holds about a file besides its contents, for the file replacing it: its permissions, and its extended
// attributes, which hold such things as a Mac's color tags. Where it isn't allowed, the contents are what matter.
void keepWhatIsKnownOf(const std::filesystem::path& from, [[maybe_unused]] const std::filesystem::path& to, int handle)
{
    struct stat existing = {};
    if (::stat(from.c_str(), &existing) != 0)
        return;

    ::fchmod(handle, existing.st_mode & 07777);

    #if defined(__APPLE__)
    ::copyfile(from.c_str(), to.c_str(), nullptr, COPYFILE_METADATA);
    #elif defined(__linux__)
    const auto listed = ::listxattr(from.c_str(), nullptr, 0);
    if (listed <= 0)
        return;

    std::string names(static_cast<std::size_t>(listed), '\0');
    const auto got = ::listxattr(from.c_str(), names.data(), names.size());
    for (std::size_t at = 0; got > 0 && at < static_cast<std::size_t>(got); at = names.find('\0', at) + 1)
    {
        const auto* const name = names.c_str() + at;
        const auto size = ::getxattr(from.c_str(), name, nullptr, 0);
        if (size < 0)
            continue;
        std::string value(static_cast<std::size_t>(size), '\0');
        if (::getxattr(from.c_str(), name, value.data(), value.size()) == size)
            ::fsetxattr(handle, name, value.data(), value.size(), 0);
    }
    #endif
}

void writeNew(const std::filesystem::path& file, std::string_view contents, std::error_code& error,
              const std::filesystem::path& keepModeOf)
{
    const int handle = ::open(file.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
    if (handle < 0)
    {
        error = lastError();
        return;
    }

    keepWhatIsKnownOf(keepModeOf, file, handle);

    while (!contents.empty())
    {
        const auto written = ::write(handle, contents.data(), contents.size());
        if (written < 0)
        {
            if (errno == EINTR)
                continue;
            error = lastError();
            break;
        }
        contents.remove_prefix(static_cast<std::size_t>(written));
    }

    if (!error)
    {
    #if defined(__APPLE__)
        // fsync on a Mac only hands the data to the drive, which may still hold it in its cache.
        if (::fcntl(handle, F_FULLFSYNC) != 0 && ::fsync(handle) != 0)
            error = lastError();
    #else
        if (::fsync(handle) != 0)
            error = lastError();
    #endif
    }

    if (::close(handle) != 0 && !error)
        error = lastError();
}

void replace(const std::filesystem::path& from, const std::filesystem::path& to, std::error_code& error)
{
    if (::rename(from.c_str(), to.c_str()) != 0)
    {
        error = lastError();
        return;
    }

    // The rename itself reaches the disk when the folder holding it is flushed. Some file systems can't flush a
    // folder, and the file is whole either way.
    const int folder = ::open(to.parent_path().empty() ? "." : to.parent_path().c_str(), O_RDONLY | O_CLOEXEC);
    if (folder >= 0)
    {
        ::fsync(folder);
        ::close(folder);
    }
}

void removeQuietly(const std::filesystem::path& file)
{
    ::unlink(file.c_str());
}
#endif

std::string readAll(const std::filesystem::path& file, std::error_code& error)
{
    std::string contents;

#if defined(_WIN32)
    HANDLE handle = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        error = lastError();
        return {};
    }

    LARGE_INTEGER size = {};
    if (GetFileSizeEx(handle, &size) && size.QuadPart > 0)
        contents.reserve(static_cast<std::size_t>(size.QuadPart));

    char buffer[65536];
    for (;;)
    {
        DWORD got = 0;
        if (!ReadFile(handle, buffer, sizeof buffer, &got, nullptr))
        {
            error = lastError();
            contents.clear();
            break;
        }
        if (got == 0)
            break;
        contents.append(buffer, got);
    }

    CloseHandle(handle);
#else
    const int handle = ::open(file.c_str(), O_RDONLY | O_CLOEXEC);
    if (handle < 0)
    {
        error = lastError();
        return {};
    }

    struct stat status = {};
    if (::fstat(handle, &status) == 0 && status.st_size > 0)
        contents.reserve(static_cast<std::size_t>(status.st_size));

    char buffer[65536];
    for (;;)
    {
        const auto got = ::read(handle, buffer, sizeof buffer);
        if (got < 0)
        {
            if (errno == EINTR)
                continue;
            error = lastError();
            contents.clear();
            break;
        }
        if (got == 0)
            break;
        contents.append(buffer, static_cast<std::size_t>(got));
    }

    ::close(handle);
#endif

    return contents;
}
}

std::string read(const std::filesystem::path& file, std::error_code& error)
{
    error.clear();
    return readAll(file, error);
}

std::string read(const std::filesystem::path& file)
{
    std::error_code error;
    auto contents = read(file, error);
    if (error)
        throw std::filesystem::filesystem_error("can't read the file", file, error);
    return contents;
}

void writeSafely(const std::filesystem::path& requested, std::string_view contents, std::error_code& error)
{
    error.clear();

    const auto file = followLinks(requested, error);
    if (error)
        return;

    if (const auto folder = file.parent_path(); !folder.empty())
    {
        std::filesystem::create_directories(folder, error);
        if (error)
            return;
    }

    const auto temporary = temporaryBeside(file);

#if defined(_WIN32)
    writeNew(temporary, contents, error);
#else
    writeNew(temporary, contents, error, file);
#endif

    if (!error)
        replace(temporary, file, error);

    if (error)
        removeQuietly(temporary);
}

void writeSafely(const std::filesystem::path& file, std::string_view contents)
{
    std::error_code error;
    writeSafely(file, contents, error);
    if (error)
        throw std::filesystem::filesystem_error("can't write the file", file, error);
}

}
