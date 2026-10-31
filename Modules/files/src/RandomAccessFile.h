#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <system_error>

namespace symbolengine::detail
{

/** A file read and written at any place in it, through the system's own calls, so that every error is the system's
    own and files past 4 GB work everywhere. */
class RandomAccessFile
{
public:
    /** Opens an existing file to read, or creates a file to write, replacing one of that name once the first thing is
        written, and untouched until then; nullptr, with the error set, when it can't. */
    static std::unique_ptr<RandomAccessFile> open(const std::filesystem::path& file, std::error_code& error);
    static std::unique_ptr<RandomAccessFile> create(const std::filesystem::path& file, std::error_code& error);

    ~RandomAccessFile();

    RandomAccessFile(const RandomAccessFile&) = delete;
    RandomAccessFile& operator=(const RandomAccessFile&) = delete;

    /** Its length in bytes. */
    std::uint64_t size(std::error_code& error) const;

    /** Reads up to count bytes from the place given, and returns how many it read: fewer only at the end. */
    std::size_t readAt(std::uint64_t at, void* bytes, std::size_t count, std::error_code& error) const;

    /** Writes count bytes at the place given, all of them or an error. */
    void writeAt(std::uint64_t at, const void* bytes, std::size_t count, std::error_code& error);

    /** For a file created: whether one of that name was there already; and whether anything has been written yet,
        which can be looked at once this has been let go of. */
    bool replacing() const noexcept;
    std::shared_ptr<const bool> written() const noexcept { return wroteSomething; }

    /** Has the system put everything written so far on disk. */
    void sync(std::error_code& error);

    /** Closes it, saying whether that went wrong; letting go of it closes it too. */
    void close(std::error_code& error);

    /** The system's own handle, which each system's code defines. */
    struct Handle;

private:
    explicit RandomAccessFile(std::unique_ptr<Handle>);

    // Empties a file being replaced, the first time anything is written.
    void startWriting(std::error_code& error);

    std::unique_ptr<Handle> handle;
    std::shared_ptr<bool> wroteSomething = std::make_shared<bool>(false);
};

}
