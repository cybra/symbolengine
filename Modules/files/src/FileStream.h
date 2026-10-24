#pragma once

#include "RandomAccessFile.h"

#include <algorithm>
#include <cstdint>
#include <system_error>

namespace symbolengine::detail
{

/** A file read or written from one place on, as the codecs' callbacks want it: each read, write and seek moves the
    place, and the first error is kept. */
struct FileStream
{
    RandomAccessFile* file = nullptr;
    std::uint64_t position = 0;
    std::uint64_t size = 0;
    std::error_code error{};

    std::size_t read(void* bytes, std::size_t count)
    {
        std::error_code failed;
        const auto got = file->readAt(position, bytes, count, failed);
        if (failed && !error)
            error = failed;
        position += got;
        return got;
    }

    bool write(const void* bytes, std::size_t count)
    {
        std::error_code failed;
        file->writeAt(position, bytes, count, failed);
        if (failed)
        {
            if (!error)
                error = failed;
            return false;
        }
        position += count;
        size = std::max(size, position);
        return true;
    }
};

}
