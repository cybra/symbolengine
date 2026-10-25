#include "AsioDetail.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace symbolengine::asio::detail
{

namespace
{
std::uint64_t load(const unsigned char* at, int bytes, bool bigEndian) noexcept
{
    std::uint64_t value = 0;
    for (int i = 0; i < bytes; ++i)
        value |= static_cast<std::uint64_t>(at[i]) << (8 * (bigEndian ? bytes - 1 - i : i));
    return value;
}

void store(unsigned char* at, int bytes, bool bigEndian, std::uint64_t value) noexcept
{
    for (int i = 0; i < bytes; ++i)
        at[i] = static_cast<unsigned char>(value >> (8 * (bigEndian ? bytes - 1 - i : i)));
}

bool littleEndianFloats(const SampleLayout& layout) noexcept
{
    return layout.floating && layout.bytes == 4 && !layout.bigEndian && std::endian::native == std::endian::little;
}
}

void read(const SampleLayout& layout, const void* from, float* to, std::size_t frames) noexcept
{
    if (layout.bytes == 0 || from == nullptr)
    {
        std::fill(to, to + frames, 0.0f);
        return;
    }
    if (littleEndianFloats(layout))
    {
        std::memcpy(to, from, frames * sizeof(float));
        return;
    }

    const auto* at = static_cast<const unsigned char*>(from);
    const auto step = static_cast<std::size_t>(layout.bytes);
    if (layout.floating)
    {
        for (std::size_t i = 0; i < frames; ++i, at += step)
        {
            const auto raw = load(at, layout.bytes, layout.bigEndian);
            to[i] = layout.bytes == 4 ? std::bit_cast<float>(static_cast<std::uint32_t>(raw))
                                      : static_cast<float>(std::bit_cast<double>(raw));
        }
        return;
    }

    // The value in the container's low bits, its sign carried up from its top bit.
    const auto shift = 64 - layout.bits;
    const auto scale = std::ldexp(1.0, 1 - layout.bits);
    for (std::size_t i = 0; i < frames; ++i, at += step)
    {
        const auto value = static_cast<std::int64_t>(load(at, layout.bytes, layout.bigEndian) << shift) >> shift;
        to[i] = static_cast<float>(static_cast<double>(value) * scale);
    }
}

void write(const SampleLayout& layout, const float* from, void* to, std::size_t frames) noexcept
{
    if (layout.bytes == 0 || to == nullptr)
        return;

    auto* at = static_cast<unsigned char*>(to);
    const auto step = static_cast<std::size_t>(layout.bytes);
    if (layout.floating)
    {
        for (std::size_t i = 0; i < frames; ++i, at += step)
        {
            const auto value = std::isnan(from[i]) ? 0.0f : from[i];
            store(at, layout.bytes, layout.bigEndian,
                  layout.bytes == 4 ? std::bit_cast<std::uint32_t>(value)
                                    : std::bit_cast<std::uint64_t>(static_cast<double>(value)));
        }
        return;
    }

    // Written as a whole number with its sign carried through the container's upper bits.
    const auto full = std::ldexp(1.0, layout.bits - 1);
    for (std::size_t i = 0; i < frames; ++i, at += step)
    {
        const auto x = static_cast<double>(from[i]);
        const auto scaled = std::isnan(x) ? 0.0 : std::clamp(std::nearbyint(x * full), -full, full - 1.0);
        store(at, layout.bytes, layout.bigEndian, static_cast<std::uint64_t>(static_cast<std::int64_t>(scaled)));
    }
}

std::vector<int> blockSizes(long smallest, long largest, long preferred, long granularity)
{
    constexpr long most = 1 << 20;
    constexpr std::size_t offered = 64;
    std::vector<int> sizes;
    const auto add = [&sizes](long size) {
        if (size > 0 && size <= most && std::find(sizes.begin(), sizes.end(), static_cast<int>(size)) == sizes.end())
            sizes.push_back(static_cast<int>(size));
    };

    add(preferred);
    if (smallest > 0 && largest >= smallest && largest <= most)
    {
        add(smallest);
        if (granularity == -1)
        {
            for (long size = 1; size <= largest; size *= 2)
                if (size >= smallest)
                    add(size);
        }
        else if (granularity > 0)
        {
            const auto steps = (largest - smallest) / granularity + 1;
            for (long size = smallest; size <= largest; size += granularity)
                if (steps <= static_cast<long>(offered) || std::has_single_bit(static_cast<unsigned long>(size)))
                    add(size);
        }
        add(largest);
    }
    if (sizes.size() > 1)
        std::sort(sizes.begin() + 1, sizes.end());
    return sizes;
}

std::string bounded(const char* text, std::size_t most)
{
    if (text == nullptr)
        return {};
    std::size_t length = 0;
    while (length < most && text[length] != '\0')
        ++length;
    return {text, length};
}

bool isUtf8(std::string_view text) noexcept
{
    std::size_t i = 0;
    while (i < text.size())
    {
        const auto lead = static_cast<unsigned char>(text[i]);
        const int extra = lead < 0x80 ? 0 : (lead >> 5) == 0x6 ? 1 : (lead >> 4) == 0xE ? 2 : (lead >> 3) == 0x1E ? 3 : -1;
        if (extra < 0 || (extra == 1 && lead < 0xC2) || text.size() - i <= static_cast<std::size_t>(extra))
            return false;
        std::uint32_t point = extra == 0 ? lead : lead & (0x3Fu >> extra);
        for (int k = 1; k <= extra; ++k)
        {
            const auto next = static_cast<unsigned char>(text[i + static_cast<std::size_t>(k)]);
            if ((next & 0xC0) != 0x80)
                return false;
            point = (point << 6) | (next & 0x3Fu);
        }
        if ((extra == 2 && (point < 0x800 || (point >= 0xD800 && point <= 0xDFFF))) ||
            (extra == 3 && (point < 0x10000 || point > 0x10FFFF)))
            return false;
        i += static_cast<std::size_t>(extra) + 1;
    }
    return true;
}

}
