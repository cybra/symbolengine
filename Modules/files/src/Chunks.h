#pragma once

#include "Bytes.h"
#include "RandomAccessFile.h"

#include <cstdint>
#include <cstring>
#include <system_error>

// The walk through a WAV's or AIFF's chunks, and what it can tell of a file whose writing was stopped.
namespace symbolengine::detail
{

/** Where the chunk after one comes. A chunk of odd length is followed by a pad byte, which some writers leave out. */
inline std::uint64_t afterChunk(RandomAccessFile& file, std::uint64_t body, std::uint64_t size, std::uint64_t fileSize,
                                std::error_code& error)
{
    auto next = body + size + (size % 2);
    if (size % 2 != 0 && next + 4 <= fileSize)
    {
        unsigned char padded[4]{}, unpadded[4]{};
        file.readAt(next, padded, 4, error);
        file.readAt(next - 1, unpadded, 4, error);
        if (!looksLikeId(padded) && looksLikeId(unpadded))
            --next;
    }
    return next;
}

/** Whether everything from a place to the end of the file is chunks, each with a name and fitting in the file, with
    less than a chunk's header left after the last. */
inline bool onlyChunksFrom(RandomAccessFile& file, std::uint64_t position, std::uint64_t fileSize, bool bigEndian,
                           std::error_code& error)
{
    while (position + 8 <= fileSize)
    {
        unsigned char header[8]{};
        if (file.readAt(position, header, sizeof header, error) < sizeof header || error || !looksLikeId(header))
            return false;

        const std::uint64_t size = bigEndian ? readBig32(header + 4) : readLittle32(header + 4);
        const auto body = position + 8;
        if (size > fileSize - body)
            return false;

        position = afterChunk(file, body, size, fileSize, error);
    }
    return true;
}

/** Whether what follows the samples a header gives, which end at end, or at padded with their pad byte, is more of them,
    written since the header last was. The file's own size has to end with them, where a finished file's other chunks
    lie inside it, and what follows has to be neither chunks nor a tag some program put past the end. */
inline bool samplesPastSizes(RandomAccessFile& file, std::uint64_t formEnd, std::uint64_t end, std::uint64_t padded,
                             std::uint64_t fileSize, bool bigEndian, std::error_code& error)
{
    if ((formEnd != end && formEnd != padded) || padded >= fileSize)
        return false;

    char tag[8]{};
    file.readAt(padded, tag, sizeof tag, error);
    if (std::memcmp(tag, "ID3", 3) == 0 || std::memcmp(tag, "TAG", 3) == 0 || std::memcmp(tag, "APETAGEX", 8) == 0)
        return false;

    return !onlyChunksFrom(file, padded, fileSize, bigEndian, error);
}

}
