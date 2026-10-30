#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <system_error>

namespace symbolengine
{

/** A file's contents read through memory, for a long file read in parts or by several threads at once: the system brings
    in what is read and lets it go again when memory is short. Read only. Whatever changes the file's length while it is
    held can end the program when a part no longer there is read, so it is for files nothing else is writing. */
class MappedFile
{
public:
    /** Holds the file's contents, or gives nothing and sets the error. An empty file holds nothing, at a null pointer. */
    static std::unique_ptr<MappedFile> open(const std::filesystem::path& file, std::error_code& error);

    ~MappedFile();
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    const void* data() const noexcept { return start; }
    std::size_t size() const noexcept { return length; }

private:
    MappedFile() = default;

    const void* start = nullptr;
    std::size_t length = 0;
};

}
