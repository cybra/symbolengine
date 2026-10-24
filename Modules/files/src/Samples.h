#pragma once

#include <cstddef>
#include <cstdint>

// How a file keeps its samples, and the conversion of its interleaved bytes to and from one float buffer per channel.
// A whole number reads as itself divided by 2 to the power of one less than its container's bits, and a float writes
// as the nearest whole number to it times that, held within the container's range.
namespace symbolengine::detail
{

enum class Encoding
{
    unsigned8,     // WAV's 8-bit, 128 for silence
    signed8,       // AIFF's 8-bit
    little16,
    big16,
    little24,
    big24,
    little32,
    big32,
    littleFloat32,
    bigFloat32,
    littleFloat64,
    bigFloat64
};

int bytesPerSample(Encoding encoding) noexcept;

/** The whole number a float writes as in the bits given, up to 32, as every whole-number format writes it. */
std::int64_t wholeFor(float sample, int bits) noexcept;

/** Reads frames of interleaved samples into each channel's buffer, starting at offset in each. */
void decode(Encoding encoding, const unsigned char* bytes, int channels, std::size_t frames, float* const* out,
            std::size_t offset) noexcept;

/** Writes frames from each channel's buffer as interleaved samples. */
void encode(Encoding encoding, const float* const* in, int channels, std::size_t frames, unsigned char* bytes) noexcept;

}
