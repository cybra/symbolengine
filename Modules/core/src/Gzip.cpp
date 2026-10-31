#include "symbolengine/core/Gzip.h"

#include <miniz.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

namespace symbolengine::gzip
{
namespace
{

constexpr unsigned char firstByte = 0x1f, secondByte = 0x8b, deflateMethod = 8;
constexpr unsigned char hasExtra = 4, hasName = 8, hasComment = 16, hasHeaderCheck = 2;
constexpr std::size_t headerSize = 10, trailerSize = 8;

void addLittleEndian(std::string& out, std::uint32_t value)
{
    for (int shift = 0; shift < 32; shift += 8)
        out += static_cast<char>((value >> shift) & 0xff);
}

std::uint32_t littleEndianAt(std::string_view bytes, std::size_t at)
{
    std::uint32_t value = 0;

    for (std::size_t i = 0; i < 4; ++i)
        value |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + i])) << (8 * i);

    return value;
}

std::uint32_t checksumOf(std::string_view bytes)
{
    return static_cast<std::uint32_t>(
        mz_crc32(MZ_CRC32_INIT, reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size()));
}

// Where the compressed data begins, past the header's optional parts; nothing if they run off the end.
std::optional<std::size_t> pastHeader(std::string_view file)
{
    if (file.size() < headerSize + trailerSize || static_cast<unsigned char>(file[0]) != firstByte ||
        static_cast<unsigned char>(file[1]) != secondByte || static_cast<unsigned char>(file[2]) != deflateMethod)
        return std::nullopt;

    const auto flags = static_cast<unsigned char>(file[3]);
    auto at = headerSize;
    const auto end = file.size() - trailerSize;

    if ((flags & hasExtra) != 0)
    {
        if (at + 2 > end)
            return std::nullopt;

        at += 2 + (static_cast<std::size_t>(static_cast<unsigned char>(file[at])) |
                   (static_cast<std::size_t>(static_cast<unsigned char>(file[at + 1])) << 8));
    }

    for (const auto text : {hasName, hasComment})
        if ((flags & text) != 0)
        {
            while (at < end && file[at] != '\0')
                ++at;

            ++at;
        }

    if ((flags & hasHeaderCheck) != 0)
        at += 2;

    if (at > end)
        return std::nullopt;

    return at;
}

}

std::string compress(std::string_view bytes)
{
    std::string file;
    file += static_cast<char>(firstByte);
    file += static_cast<char>(secondByte);
    file += static_cast<char>(deflateMethod);
    file.append(7, '\0');
    file[9] = static_cast<char>(0xff);   // no system said, as RFC 1952 allows

    std::size_t compressedSize = 0;
    const auto flags =
        tdefl_create_comp_flags_from_zip_params(MZ_BEST_COMPRESSION, -MZ_DEFAULT_WINDOW_BITS, MZ_DEFAULT_STRATEGY);
    auto* compressed = tdefl_compress_mem_to_heap(bytes.data(), bytes.size(), &compressedSize, static_cast<int>(flags));

    if (compressed != nullptr)
        file.append(static_cast<const char*>(compressed), compressedSize);

    mz_free(compressed);
    addLittleEndian(file, checksumOf(bytes));
    addLittleEndian(file, static_cast<std::uint32_t>(bytes.size()));
    return file;
}

std::optional<std::string> decompress(std::string_view file, std::size_t limit)
{
    const auto start = pastHeader(file);

    if (!start.has_value())
        return std::nullopt;

    // The most it may hold: its stated length, which is modulo 2^32, wherever that can't have wrapped within the limit.
    const auto stated = littleEndianAt(file, file.size() - 4);
    const auto most = limit <= 0xFFFFFFFFu ? std::min<std::size_t>(limit, stated) : limit;

    const auto compressed = file.substr(*start, file.size() - trailerSize - *start);
    const auto* in = reinterpret_cast<const unsigned char*>(compressed.data());

    // Grown as it fills, with a byte's room past the most, so more than that shows as more to come.
    constexpr std::size_t first = 64 * 1024;
    std::string bytes(std::min(first, most) + 1, '\0');
    std::size_t taken = 0, made = 0;
    tinfl_decompressor inflater;
    tinfl_init(&inflater);

    for (;;)
    {
        auto inSize = compressed.size() - taken;
        auto outSize = bytes.size() - made;
        auto* out = reinterpret_cast<unsigned char*>(bytes.data());
        const auto status = tinfl_decompress(&inflater, in + taken, &inSize, out, out + made, &outSize,
                                             TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
        taken += inSize;
        made += outSize;

        if (status == TINFL_STATUS_DONE)
            break;
        if (status != TINFL_STATUS_HAS_MORE_OUTPUT || bytes.size() > most)
            return std::nullopt;

        bytes.resize((bytes.size() > most / 2 ? most : bytes.size() * 2) + 1);
    }

    bytes.resize(made);

    if (taken != compressed.size() || made > most ||
        checksumOf(bytes) != littleEndianAt(file, file.size() - trailerSize) ||
        static_cast<std::uint32_t>(made) != stated)
        return std::nullopt;

    return bytes;
}

}
