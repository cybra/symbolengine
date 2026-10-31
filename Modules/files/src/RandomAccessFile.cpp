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
    bool existed = false;
    bool emptyFirst = false;
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
    HANDLE handle = CreateFileW(file.c_str(), access, share, nullptr, writing ? OPEN_ALWAYS : OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        error = lastError();
        return nullptr;
    }

    auto result = std::make_unique<RandomAccessFile::Handle>();
    result->file = handle;
    result->existed = writing && GetLastError() == ERROR_ALREADY_EXISTS;
    result->emptyFirst = result->existed;
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

void RandomAccessFile::startWriting(std::error_code& error)
{
    if (handle->emptyFirst)
    {
        handle->emptyFirst = false;
        const LARGE_INTEGER start{};
        if (!SetFilePointerEx(handle->file, start, nullptr, FILE_BEGIN) || !SetEndOfFile(handle->file))
        {
            error = lastError();
            return;
        }
    }
    *wroteSomething = true;
}

void RandomAccessFile::writeAt(std::uint64_t at, const void* bytes, std::size_t count, std::error_code& error)
{
    startWriting(error);
    if (error)
        return;

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
        if (put == 0)
        {
            error = std::error_code(ERROR_WRITE_FAULT, std::system_category());
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
    bool existed = false;
    bool emptyFirst = false;
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
    const auto opened = [&file](int flags) {
        int descriptor = -1;
        do
            descriptor = ::open(file.c_str(), flags | O_CLOEXEC, 0666);
        while (descriptor < 0 && errno == EINTR);
        return descriptor;
    };

    auto descriptor = opened(writing ? O_RDWR : O_RDONLY);
    const auto existed = writing && descriptor >= 0;
    if (writing && descriptor < 0 && errno == ENOENT)
        descriptor = opened(O_RDWR | O_CREAT);

    if (descriptor < 0)
    {
        error = lastError();
        return nullptr;
    }

    auto result = std::make_unique<RandomAccessFile::Handle>();
    result->file = descriptor;
    result->existed = existed;

    // Only a file of bytes can be emptied, where a name may also be a device's or a pipe's.
    struct stat status{};
    result->emptyFirst = existed && fstat(descriptor, &status) == 0 && S_ISREG(status.st_mode);
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

void RandomAccessFile::startWriting(std::error_code& error)
{
    if (handle->emptyFirst)
    {
        handle->emptyFirst = false;
        int result = 0;
        do
            result = ftruncate(handle->file, 0);
        while (result != 0 && errno == EINTR);

        if (result != 0)
        {
            error = lastError();
            return;
        }
    }
    *wroteSomething = true;
}

void RandomAccessFile::writeAt(std::uint64_t at, const void* bytes, std::size_t count, std::error_code& error)
{
    startWriting(error);
    if (error)
        return;

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
        if (put == 0)
        {
            error = std::make_error_code(std::errc::io_error);
            return;
        }
        done += static_cast<std::size_t>(put);
    }
}

void RandomAccessFile::sync(std::error_code& error)
{
    #if defined(__APPLE__)
    // fsync on a Mac only hands the data to the drive, which may still hold it in its cache when the power goes. Not
    // every volume can do more, so a refusal falls back on it.
    if (::fcntl(handle->file, F_FULLFSYNC) == 0)
        return;
    #endif

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

bool RandomAccessFile::replacing() const noexcept
{
    return handle->existed;
}

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
