#include "symbolengine/core/Sha256.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#if defined(_WIN32)
    #include <share.h>
#endif

namespace symbolengine
{

namespace
{
constexpr std::array<std::uint32_t, 8> initialState = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                                       0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

constexpr std::array<std::uint32_t, 64> rounds = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

constexpr std::uint32_t rotate(std::uint32_t x, int n) noexcept
{
    return (x >> n) | (x << (32 - n));
}

struct FileCloser
{
    void operator()(std::FILE* file) const noexcept { std::fclose(file); }
};
}

Sha256::Sha256() noexcept : state(initialState) {}

void Sha256::compress(const std::uint8_t* chunk) noexcept
{
    std::array<std::uint32_t, 64> w{};
    for (std::size_t i = 0; i < 16; ++i)
        w[i] = static_cast<std::uint32_t>(chunk[i * 4]) << 24 | static_cast<std::uint32_t>(chunk[i * 4 + 1]) << 16 |
               static_cast<std::uint32_t>(chunk[i * 4 + 2]) << 8 | static_cast<std::uint32_t>(chunk[i * 4 + 3]);
    for (std::size_t i = 16; i < 64; ++i)
    {
        const auto s0 = rotate(w[i - 15], 7) ^ rotate(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const auto s1 = rotate(w[i - 2], 17) ^ rotate(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    auto [a, b, c, d, e, f, g, h] = state;
    for (std::size_t i = 0; i < 64; ++i)
    {
        const auto s1 = rotate(e, 6) ^ rotate(e, 11) ^ rotate(e, 25);
        const auto choice = (e & f) ^ (~e & g);
        const auto first = h + s1 + choice + rounds[i] + w[i];
        const auto s0 = rotate(a, 2) ^ rotate(a, 13) ^ rotate(a, 22);
        const auto majority = (a & b) ^ (a & c) ^ (b & c);
        const auto second = s0 + majority;

        h = g;
        g = f;
        f = e;
        e = d + first;
        d = c;
        c = b;
        b = a;
        a = first + second;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

void Sha256::update(const void* data, std::size_t size) noexcept
{
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    length += size;

    if (used > 0)
    {
        const auto taken = std::min(size, block.size() - used);
        std::memcpy(block.data() + used, bytes, taken);
        used += taken;
        bytes += taken;
        size -= taken;

        if (used < block.size())
            return;

        compress(block.data());
        used = 0;
    }

    for (; size >= block.size(); bytes += block.size(), size -= block.size())
        compress(bytes);

    if (size > 0)
    {
        std::memcpy(block.data(), bytes, size);
        used = size;
    }
}

Sha256::Digest Sha256::finish() noexcept
{
    const auto bits = length * 8;

    block[used++] = 0x80;
    if (used > 56)
    {
        std::fill(block.begin() + static_cast<std::ptrdiff_t>(used), block.end(), std::uint8_t{0});
        compress(block.data());
        used = 0;
    }
    std::fill(block.begin() + static_cast<std::ptrdiff_t>(used), block.begin() + 56, std::uint8_t{0});
    for (std::size_t i = 0; i < 8; ++i)
        block[56 + i] = static_cast<std::uint8_t>(bits >> (56 - 8 * i));
    compress(block.data());

    Digest digest{};
    for (std::size_t i = 0; i < 8; ++i)
        for (std::size_t j = 0; j < 4; ++j)
            digest[i * 4 + j] = static_cast<std::uint8_t>(state[i] >> (24 - 8 * j));

    state = initialState;
    used = 0;
    length = 0;
    return digest;
}

Sha256::Digest Sha256::of(std::string_view bytes) noexcept
{
    Sha256 hash;
    hash.update(bytes);
    return hash.finish();
}

Sha256::Digest Sha256::ofFile(const std::filesystem::path& file, std::error_code& error)
{
    error.clear();

#if defined(_WIN32)
    // Sharing with whoever else has it open, as every other read of a file here does.
    std::FILE* opened = _wfsopen(file.c_str(), L"rb", _SH_DENYNO);
    const int failure = opened == nullptr ? errno : 0;
#else
    std::FILE* opened = std::fopen(file.c_str(), "rb");
    const int failure = opened == nullptr ? errno : 0;
#endif
    std::unique_ptr<std::FILE, FileCloser> stream(opened);
    if (!stream)
    {
        error = std::error_code(failure != 0 ? failure : EIO, std::generic_category());
        return {};
    }

    Sha256 hash;
    std::vector<char> buffer(1 << 16);
    for (;;)
    {
        const auto read = std::fread(buffer.data(), 1, buffer.size(), stream.get());
        hash.update(buffer.data(), read);
        if (read < buffer.size())
            break;
    }

    if (std::ferror(stream.get()))
    {
        error = std::make_error_code(std::errc::io_error);
        return {};
    }

    return hash.finish();
}

Sha256::Digest Sha256::ofFile(const std::filesystem::path& file)
{
    std::error_code error;
    auto digest = ofFile(file, error);
    if (error)
        throw std::filesystem::filesystem_error("can't read the file to hash it", file, error);
    return digest;
}

std::string Sha256::hex(const Digest& digest)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string text;
    text.reserve(digest.size() * 2);
    for (const auto byte : digest)
    {
        text += digits[byte >> 4];
        text += digits[byte & 0xF];
    }
    return text;
}

}
