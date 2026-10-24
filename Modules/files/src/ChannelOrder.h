#pragma once

#include <cstdint>

// The order Ogg Vorbis keeps channels in, which Opus keeps too, set against WAV's, which every reader and writer gives
// channels in: three channels are left, center and right, and from five on the center comes second and the LFE last.
namespace symbolengine::detail
{

/** The speakers usually meant by each number of channels up to 7.1, as WAVE_FORMAT_EXTENSIBLE's mask, or 0 past it. */
inline std::uint32_t usualMask(int channels) noexcept
{
    switch (channels)
    {
        case 1: return 0x4;
        case 2: return 0x3;
        case 3: return 0x7;
        case 4: return 0x33;
        case 5: return 0x37;
        case 6: return 0x3F;
        case 7: return 0x13F;
        case 8: return 0x63F;
        default: return 0;
    }
}

/** The Vorbis channel holding a file's channel of WAV's order, for a file of so many channels. */
inline int vorbisChannelFor(int channels, int channel) noexcept
{
    static constexpr int three[]{0, 2, 1};
    static constexpr int five[]{0, 2, 1, 3, 4};
    static constexpr int six[]{0, 2, 1, 5, 3, 4};
    static constexpr int seven[]{0, 2, 1, 6, 5, 3, 4};
    static constexpr int eight[]{0, 2, 1, 7, 5, 6, 3, 4};

    switch (channels)
    {
        case 3: return three[channel];
        case 5: return five[channel];
        case 6: return six[channel];
        case 7: return seven[channel];
        case 8: return eight[channel];
        default: return channel;
    }
}

}
