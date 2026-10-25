#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

// What the ASIO layer works out without a driver, kept apart from it so it is checked on every computer.
namespace symbolengine::asio::detail
{

/** How a sample of one of ASIO's types lies in a driver's buffer: its container's bytes, the bits of a whole number
    that hold its value, low in the container, whether it is floating point, and its byte order. bytes is 0 for a
    type that isn't converted, DSD's. */
struct SampleLayout
{
    int bytes = 0;
    int bits = 0;
    bool floating = false;
    bool bigEndian = false;
};

/** Reads frames from a driver's buffer as floats, full scale ±1. */
void read(const SampleLayout& layout, const void* from, float* to, std::size_t frames) noexcept;

/** Writes floats to a driver's buffer, each rounded to the nearest step and held to the type's range; a value that
    isn't a number writes 0. */
void write(const SampleLayout& layout, const float* from, void* to, std::size_t frames) noexcept;

/** The block sizes a driver offers, from what it gives when asked for its buffer sizes: its preferred size first,
    then the others from the smallest. A granularity of -1 is powers of two, one above 0 each step of it, up to 64
    sizes, or the steps that are powers of two where there are more, and 0 the smallest and largest alone. */
std::vector<int> blockSizes(long smallest, long largest, long preferred, long granularity);

/** A string a driver wrote, up to its first zero or most bytes, whichever comes first. */
std::string bounded(const char* text, std::size_t most);

bool isUtf8(std::string_view text) noexcept;

}
