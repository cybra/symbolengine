#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

namespace symbolengine
{

/** SHA-256 (FIPS 180-4), given its bytes in pieces of any size, as a download arrives or a file is read. */
class Sha256
{
public:
    using Digest = std::array<std::uint8_t, 32>;

    Sha256() noexcept;

    void update(const void* data, std::size_t size) noexcept;
    void update(std::string_view bytes) noexcept { update(bytes.data(), bytes.size()); }

    /** The digest of everything given since it was made or last finished, after which it starts again. */
    Digest finish() noexcept;

    /** The digest of some bytes. */
    static Digest of(std::string_view bytes) noexcept;

    /** The digest of a file's contents, read a block at a time, so a file of any size never sits in memory whole.
        Throws std::filesystem::filesystem_error, or, given a std::error_code, sets it instead. */
    static Digest ofFile(const std::filesystem::path& file);
    static Digest ofFile(const std::filesystem::path& file, std::error_code& error);

    /** A digest in lower-case hexadecimal, as checksum files and release manifests write it. */
    static std::string hex(const Digest& digest);

private:
    std::array<std::uint32_t, 8> state;
    std::array<std::uint8_t, 64> block{};
    std::size_t used = 0;
    std::uint64_t length = 0;

    void compress(const std::uint8_t* chunk) noexcept;
};

}
