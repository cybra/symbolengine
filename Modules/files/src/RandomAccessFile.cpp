#include "RandomAccessFile.h"

#include <algorithm>

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#else
    #include <cerrno>
    #include <fcntl.h>
    #include <sys/stat.h>
    #include <unistd.h>
#endif

namespace symbolengine::detail
{

#if defined(_WIN32)

struct RandomAccessFile::Handle
{
    HANDLE file = INVALID_HANDLE_VALUE;
};

namespace
{
std::error_code lastError()
{
    return std::error_code(static_cast<int>(GetLastError()), std::system_category());
}

std::unique_ptr<RandomAccessFile::Handle> openHandle(const std::filesystem::path& file, bool writing,
                                                     std::error_code& error)
{
    const auto access = writing ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ;
    const auto share = writing ? FILE_SHARE_READ : FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    HANDLE handle = CreateFileW(file.c_str(), access, share, nullptr, writing ? CREATE_ALWAYS : OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        error = lastError();
        return nullptr;
    }

    auto result = std::make_unique<RandomAccessFile::Handle>();
    result->file = handle;
    return result;
}

OVERLAPPED placeAt(std::uint64_t at)
{
    OVERLAPPED place{};
    place.Offset = static_cast<DWORD>(at);
    place.OffsetHigh = static_cast<DWORD>(at >> 32);
    return place;
}
}

std::uint64_t RandomAccessFile::size(std::error_code& error) const
{
    LARGE_INTEGER length{};
    if (!GetFileSizeEx(handle->file, &length))
    {
        error = lastError();
        return 0;
    }
    return static_cast<std::uint64_t>(length.QuadPart);
}

std::size_t RandomAccessFile::readAt(std::uint64_t at, void* bytes, std::size_t count, std::error_code& error) const
{
    std::size_t done = 0;
    while (done < count)
    {
        const auto ask = static_cast<DWORD>(std::min<std::size_t>(count - done, 1u << 30));
        auto place = placeAt(at + done);
        DWORD got = 0;
        if (!ReadFile(handle->file, static_cast<char*>(bytes) + done, ask, &got, &place))
        {
            if (GetLastError() == ERROR_HANDLE_EOF)
                break;
            error = lastError();
            break;
        }
        if (got == 0)
            break;
        done += got;
    }
    return done;
}

void RandomAccessFile::writeAt(std::uint64_t at, const void* bytes, std::size_t count, std::error_code& error)
{
    std::size_t done = 0;
    while (done < count)
    {
        const auto ask = static_cast<DWORD>(std::min<std::size_t>(count - done, 1u << 30));
        auto place = placeAt(at + done);
        DWORD put = 0;
        if (!WriteFile(handle->file, static_cast<const char*>(bytes) + done, ask, &put, &place))
        {
            error = lastError();
            return;
        }
        done += put;
    }
}

void RandomAccessFile::sync(std::error_code& error)
{
    if (!FlushFileBuffers(handle->file))
        error = lastError();
}

void RandomAccessFile::close(std::error_code& error)
{
    if (handle->file != INVALID_HANDLE_VALUE && !CloseHandle(handle->file))
        error = lastError();
    handle->file = INVALID_HANDLE_VALUE;
}

RandomAccessFile::~RandomAccessFile()
{
    if (handle != nullptr && handle->file != INVALID_HANDLE_VALUE)
        CloseHandle(handle->file);
}

#else

struct RandomAccessFile::Handle
{
    int file = -1;
};

namespace
{
std::error_code lastError()
{
    return std::error_code(errno, std::generic_category());
}

std::unique_ptr<RandomAccessFile::Handle> openHandle(const std::filesystem::path& file, bool writing,
                                                     std::error_code& error)
{
    int descriptor = -1;
    do
        descriptor = writing ? ::open(file.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0666)
                             : ::open(file.c_str(), O_RDONLY | O_CLOEXEC);
    while (descriptor < 0 && errno == EINTR);

    if (descriptor < 0)
    {
        error = lastError();
        return nullptr;
    }

    auto result = std::make_unique<RandomAccessFile::Handle>();
    result->file = descriptor;
    return result;
}
}

std::uint64_t RandomAccessFile::size(std::error_code& error) const
{
    struct stat status{};
    if (fstat(handle->file, &status) != 0)
    {
        error = lastError();
        return 0;
    }
    return static_cast<std::uint64_t>(status.st_size);
}

std::size_t RandomAccessFile::readAt(std::uint64_t at, void* bytes, std::size_t count, std::error_code& error) const
{
    std::size_t done = 0;
    while (done < count)
    {
        const auto ask = std::min<std::size_t>(count - done, 1u << 30);
        const auto got = pread(handle->file, static_cast<char*>(bytes) + done, ask, static_cast<off_t>(at + done));
        if (got < 0)
        {
            if (errno == EINTR)
                continue;
            error = lastError();
            break;
        }
        if (got == 0)
            break;
        done += static_cast<std::size_t>(got);
    }
    return done;
}

void RandomAccessFile::writeAt(std::uint64_t at, const void* bytes, std::size_t count, std::error_code& error)
{
    std::size_t done = 0;
    while (done < count)
    {
        const auto ask = std::min<std::size_t>(count - done, 1u << 30);
        const auto put = pwrite(handle->file, static_cast<const char*>(bytes) + done, ask, static_cast<off_t>(at + done));
        if (put < 0)
        {
            if (errno == EINTR)
                continue;
            error = lastError();
            return;
        }
        done += static_cast<std::size_t>(put);
    }
}

void RandomAccessFile::sync(std::error_code& error)
{
    int result = 0;
    do
        result = fsync(handle->file);
    while (result != 0 && errno == EINTR);

    if (result != 0)
        error = lastError();
}

void RandomAccessFile::close(std::error_code& error)
{
    if (handle->file >= 0 && ::close(handle->file) != 0)
        error = lastError();
    handle->file = -1;
}

RandomAccessFile::~RandomAccessFile()
{
    if (handle != nullptr && handle->file >= 0)
        ::close(handle->file);
}

#endif

RandomAccessFile::RandomAccessFile(std::unique_ptr<Handle> h) : handle(std::move(h)) {}

std::unique_ptr<RandomAccessFile> RandomAccessFile::open(const std::filesystem::path& file, std::error_code& error)
{
    auto handle = openHandle(file, false, error);
    return handle != nullptr ? std::unique_ptr<RandomAccessFile>(new RandomAccessFile(std::move(handle))) : nullptr;
}

std::unique_ptr<RandomAccessFile> RandomAccessFile::create(const std::filesystem::path& file, std::error_code& error)
{
    auto handle = openHandle(file, true, error);
    return handle != nullptr ? std::unique_ptr<RandomAccessFile>(new RandomAccessFile(std::move(handle))) : nullptr;
}

}
