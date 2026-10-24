#pragma once

#include <cstdint>
#include <cstring>
#include <string>

// Numbers in files, little-endian as WAV keeps them and big-endian as AIFF does, read from bytes and added to them.
namespace symbolengine::detail
{

inline std::uint16_t readLittle16(const unsigned char* p) noexcept
{
    return static_cast<std::uint16_t>(p[0] | p[1] << 8);
}

inline std::uint32_t readLittle32(const unsigned char* p) noexcept
{
    return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 |
           static_cast<std::uint32_t>(p[2]) << 16 | static_cast<std::uint32_t>(p[3]) << 24;
}

inline std::uint64_t readLittle64(const unsigned char* p) noexcept
{
    return static_cast<std::uint64_t>(readLittle32(p)) | static_cast<std::uint64_t>(readLittle32(p + 4)) << 32;
}

inline std::uint16_t readBig16(const unsigned char* p) noexcept
{
    return static_cast<std::uint16_t>(p[0] << 8 | p[1]);
}

inline std::uint32_t readBig32(const unsigned char* p) noexcept
{
    return static_cast<std::uint32_t>(p[0]) << 24 | static_cast<std::uint32_t>(p[1]) << 16 |
           static_cast<std::uint32_t>(p[2]) << 8 | static_cast<std::uint32_t>(p[3]);
}

inline void addLittle16(std::string& bytes, std::uint32_t value)
{
    bytes += static_cast<char>(value & 0xFF);
    bytes += static_cast<char>(value >> 8 & 0xFF);
}

inline void addLittle32(std::string& bytes, std::uint32_t value)
{
    addLittle16(bytes, value & 0xFFFF);
    addLittle16(bytes, value >> 16);
}

inline void addLittle64(std::string& bytes, std::uint64_t value)
{
    addLittle32(bytes, static_cast<std::uint32_t>(value));
    addLittle32(bytes, static_cast<std::uint32_t>(value >> 32));
}

inline void addBig16(std::string& bytes, std::uint32_t value)
{
    bytes += static_cast<char>(value >> 8 & 0xFF);
    bytes += static_cast<char>(value & 0xFF);
}

inline void addBig32(std::string& bytes, std::uint32_t value)
{
    addBig16(bytes, value >> 16);
    addBig16(bytes, value & 0xFFFF);
}

inline void setLittle32(std::string& bytes, std::size_t at, std::uint32_t value)
{
    std::string four;
    addLittle32(four, value);
    bytes.replace(at, 4, four);
}

inline const unsigned char* bytesOf(const std::string& bytes) noexcept
{
    return reinterpret_cast<const unsigned char*>(bytes.data());
}

/** Whether a chunk describes the samples or lays out a WAV or an AIFF, which a reader takes in and a writer writes
    itself, leaving them out of AudioInfo's chunks and of those a writer is given, in both formats alike. */
inline bool isLayoutChunk(const std::string& id)
{
    return id == "RIFF" || id == "RF64" || id == "BW64" || id == "fmt " || id == "fact" || id == "data" || id == "ds64" ||
           id == "FORM" || id == "COMM" || id == "SSND" || id == "FVER" || id == "JUNK" || id == "junk" || id == "PAD " ||
           id == "FLLR";
}

/** Whether four bytes could be a chunk's id: printable ASCII, as every id is. */
inline bool looksLikeId(const unsigned char* p) noexcept
{
    for (int i = 0; i < 4; ++i)
        if (p[i] < 0x20 || p[i] > 0x7E)
            return false;
    return true;
}

}
