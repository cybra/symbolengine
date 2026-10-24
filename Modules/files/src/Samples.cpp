#include "Samples.h"

#include <bit>
#include <cmath>

namespace symbolengine::detail
{

namespace
{
std::uint32_t little32(const unsigned char* p) noexcept
{
    return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 |
           static_cast<std::uint32_t>(p[2]) << 16 | static_cast<std::uint32_t>(p[3]) << 24;
}

std::uint32_t big32(const unsigned char* p) noexcept
{
    return static_cast<std::uint32_t>(p[0]) << 24 | static_cast<std::uint32_t>(p[1]) << 16 |
           static_cast<std::uint32_t>(p[2]) << 8 | static_cast<std::uint32_t>(p[3]);
}

std::uint64_t little64(const unsigned char* p) noexcept
{
    return static_cast<std::uint64_t>(little32(p)) | static_cast<std::uint64_t>(little32(p + 4)) << 32;
}

std::uint64_t big64(const unsigned char* p) noexcept
{
    return static_cast<std::uint64_t>(big32(p)) << 32 | static_cast<std::uint64_t>(big32(p + 4));
}

// One sample's bytes as a float.
float sampleAt(Encoding encoding, const unsigned char* p) noexcept
{
    switch (encoding)
    {
        case Encoding::unsigned8:
            return static_cast<float>(static_cast<int>(p[0]) - 128) * (1.0f / 128.0f);
        case Encoding::signed8:
            return static_cast<float>(static_cast<std::int8_t>(p[0])) * (1.0f / 128.0f);
        case Encoding::little16:
            return static_cast<float>(static_cast<std::int16_t>(p[0] | p[1] << 8)) * (1.0f / 32768.0f);
        case Encoding::big16:
            return static_cast<float>(static_cast<std::int16_t>(p[0] << 8 | p[1])) * (1.0f / 32768.0f);
        case Encoding::little24:
        {
            const auto bits = static_cast<std::uint32_t>(p[0]) << 8 | static_cast<std::uint32_t>(p[1]) << 16 |
                              static_cast<std::uint32_t>(p[2]) << 24;
            return static_cast<float>(static_cast<std::int32_t>(bits) >> 8) * (1.0f / 8388608.0f);
        }
        case Encoding::big24:
        {
            const auto bits = static_cast<std::uint32_t>(p[0]) << 24 | static_cast<std::uint32_t>(p[1]) << 16 |
                              static_cast<std::uint32_t>(p[2]) << 8;
            return static_cast<float>(static_cast<std::int32_t>(bits) >> 8) * (1.0f / 8388608.0f);
        }
        case Encoding::little32:
            return static_cast<float>(static_cast<std::int32_t>(little32(p))) * (1.0f / 2147483648.0f);
        case Encoding::big32:
            return static_cast<float>(static_cast<std::int32_t>(big32(p))) * (1.0f / 2147483648.0f);
        case Encoding::littleFloat32:
            return std::bit_cast<float>(little32(p));
        case Encoding::bigFloat32:
            return std::bit_cast<float>(big32(p));
        case Encoding::littleFloat64:
            return static_cast<float>(std::bit_cast<double>(little64(p)));
        case Encoding::bigFloat64:
            return static_cast<float>(std::bit_cast<double>(big64(p)));
    }
    return 0.0f;
}

}

// The nearest whole number to a float times 2^(bits - 1), held within the range of the bits given; nothing that isn't a
// number writes as silence.
std::int64_t wholeFor(float sample, int bits) noexcept
{
    if (std::isnan(sample))
        return 0;

    const auto scale = static_cast<double>(std::int64_t{1} << (bits - 1));
    const auto scaled = static_cast<double>(sample) * scale;
    if (scaled <= -scale)
        return -static_cast<std::int64_t>(scale);
    if (scaled >= scale - 1.0)
        return static_cast<std::int64_t>(scale) - 1;
    return static_cast<std::int64_t>(std::nearbyint(scaled));
}

namespace
{
void putLittle(unsigned char* p, std::uint64_t value, int bytes) noexcept
{
    for (int i = 0; i < bytes; ++i)
        p[i] = static_cast<unsigned char>(value >> (8 * i));
}

void putBig(unsigned char* p, std::uint64_t value, int bytes) noexcept
{
    for (int i = 0; i < bytes; ++i)
        p[i] = static_cast<unsigned char>(value >> (8 * (bytes - 1 - i)));
}

void putSample(Encoding encoding, unsigned char* p, float sample) noexcept
{
    switch (encoding)
    {
        case Encoding::unsigned8:
            p[0] = static_cast<unsigned char>(wholeFor(sample, 8) + 128);
            return;
        case Encoding::signed8:
            p[0] = static_cast<unsigned char>(wholeFor(sample, 8));
            return;
        case Encoding::little16:
            putLittle(p, static_cast<std::uint64_t>(wholeFor(sample, 16)), 2);
            return;
        case Encoding::big16:
            putBig(p, static_cast<std::uint64_t>(wholeFor(sample, 16)), 2);
            return;
        case Encoding::little24:
            putLittle(p, static_cast<std::uint64_t>(wholeFor(sample, 24)), 3);
            return;
        case Encoding::big24:
            putBig(p, static_cast<std::uint64_t>(wholeFor(sample, 24)), 3);
            return;
        case Encoding::little32:
            putLittle(p, static_cast<std::uint64_t>(wholeFor(sample, 32)), 4);
            return;
        case Encoding::big32:
            putBig(p, static_cast<std::uint64_t>(wholeFor(sample, 32)), 4);
            return;
        case Encoding::littleFloat32:
            putLittle(p, std::bit_cast<std::uint32_t>(sample), 4);
            return;
        case Encoding::bigFloat32:
            putBig(p, std::bit_cast<std::uint32_t>(sample), 4);
            return;
        case Encoding::littleFloat64:
            putLittle(p, std::bit_cast<std::uint64_t>(static_cast<double>(sample)), 8);
            return;
        case Encoding::bigFloat64:
            putBig(p, std::bit_cast<std::uint64_t>(static_cast<double>(sample)), 8);
            return;
    }
}
}

int bytesPerSample(Encoding encoding) noexcept
{
    switch (encoding)
    {
        case Encoding::unsigned8:
        case Encoding::signed8:
            return 1;
        case Encoding::little16:
        case Encoding::big16:
            return 2;
        case Encoding::little24:
        case Encoding::big24:
            return 3;
        case Encoding::little32:
        case Encoding::big32:
        case Encoding::littleFloat32:
        case Encoding::bigFloat32:
            return 4;
        case Encoding::littleFloat64:
        case Encoding::bigFloat64:
            return 8;
    }
    return 1;
}

void decode(Encoding encoding, const unsigned char* bytes, int channels, std::size_t frames, float* const* out,
            std::size_t offset) noexcept
{
    const auto size = static_cast<std::size_t>(bytesPerSample(encoding));
    const auto stride = size * static_cast<std::size_t>(channels);

    for (int channel = 0; channel < channels; ++channel)
    {
        const auto* p = bytes + size * static_cast<std::size_t>(channel);
        auto* destination = out[channel] + offset;
        for (std::size_t frame = 0; frame < frames; ++frame, p += stride)
            destination[frame] = sampleAt(encoding, p);
    }
}

void encode(Encoding encoding, const float* const* in, int channels, std::size_t frames, unsigned char* bytes) noexcept
{
    const auto size = static_cast<std::size_t>(bytesPerSample(encoding));
    const auto stride = size * static_cast<std::size_t>(channels);

    for (int channel = 0; channel < channels; ++channel)
    {
        auto* p = bytes + size * static_cast<std::size_t>(channel);
        const auto* source = in[channel];
        for (std::size_t frame = 0; frame < frames; ++frame, p += stride)
            putSample(encoding, p, source[frame]);
    }
}

}
