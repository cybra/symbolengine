#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

// Core Audio's arithmetic, apart from Core Audio itself so the tests check it on any computer.
namespace symbolengine::coreAudio::detail
{

/** Host time in ticks as nanoseconds by the machine's timebase. On Apple Silicon a tick is 125/3 nanoseconds, not
    one, which Intel Macs hide. */
inline std::int64_t nanosecondsFromTicks(std::uint64_t ticks, std::uint32_t numerator, std::uint32_t denominator)
{
    if (denominator == 0)
        return 0;
    const auto whole = ticks / denominator, rest = ticks % denominator;
    return static_cast<std::int64_t>(whole * numerator + rest * numerator / denominator);
}

struct Range
{
    double minimum = 0.0;
    double maximum = 0.0;
};

/** The rates a device offers from the ranges Core Audio gives: each range of one rate as it is, the standard rates
    inside each wider range, within 2 Hz, and the rate it runs at now. */
inline std::vector<double> ratesWithin(const std::vector<Range>& ranges, double current)
{
    static constexpr double standard[] = {8000,  11025,  12000,  16000,  22050,  24000,  32000,  44100,  48000, 64000,
                                          88200, 96000, 128000, 176400, 192000, 352800, 384000, 705600, 768000};
    std::vector<double> rates;
    for (const auto& range : ranges)
    {
        if (range.maximum - range.minimum < 1.0)
        {
            rates.push_back(range.minimum);
            continue;
        }
        for (const auto rate : standard)
            if (rate >= range.minimum - 2.0 && rate <= range.maximum + 2.0)
                rates.push_back(rate);
    }
    if (current > 0.0)
        rates.push_back(current);
    std::sort(rates.begin(), rates.end());
    rates.erase(std::unique(rates.begin(), rates.end(), [](double a, double b) { return std::abs(a - b) < 0.5; }),
                rates.end());
    return rates;
}

/** The block sizes a device offers within its range: powers of two, and the one it runs at now. */
inline std::vector<int> blockSizesWithin(double minimum, double maximum, int current)
{
    std::vector<int> sizes;
    for (int size = 16; size <= 16384; size *= 2)
        if (size >= minimum && size <= maximum)
            sizes.push_back(size);
    if (current > 0 && std::find(sizes.begin(), sizes.end(), current) == sizes.end())
        sizes.push_back(current);
    std::sort(sizes.begin(), sizes.end());
    return sizes;
}

struct Place
{
    int buffer = -1;
    int channel = 0;
    int channels = 0;
};

/** Where each of the device's channels, numbered across all its streams, lies: which buffer of the cycle's buffer
    list, which channel within it and how many it interleaves, from the channel count of each of its streams in
    order. A channel past the last stream is at buffer -1. */
inline std::vector<Place> placesOf(const std::vector<int>& requested, const std::vector<int>& streamChannels)
{
    std::vector<Place> places;
    for (const auto wanted : requested)
    {
        Place place;
        int first = 0;
        for (std::size_t b = 0; b < streamChannels.size(); ++b)
        {
            if (wanted >= first && wanted < first + streamChannels[b])
            {
                place = {static_cast<int>(b), wanted - first, streamChannels[b]};
                break;
            }
            first += streamChannels[b];
        }
        places.push_back(place);
    }
    return places;
}

/** Reads one channel out of a buffer Core Audio gave, going by the buffer as it is. A device can change its channels some
    cycles before the stream hears of it, as AirPods going mono do, so a channel it no longer has is false, with nothing read,
    and a buffer shorter than the cycle is read as far as it goes, the rest silence. */
inline bool readChannel(const float* from, std::size_t bytes, int across, int channel, std::size_t frames, float* to)
{
    if (from == nullptr || across <= 0 || channel < 0 || channel >= across)
        return false;
    const auto step = static_cast<std::size_t>(across);
    const auto within = std::min(frames, bytes / (step * sizeof(float)));
    for (std::size_t i = 0; i < within; ++i)
        to[i] = from[i * step + static_cast<std::size_t>(channel)];
    std::fill(to + within, to + frames, 0.0f);
    return true;
}

/** Writes one channel into a buffer Core Audio gave, on the same terms, and no further than it goes. */
inline bool writeChannel(const float* from, std::size_t frames, float* into, std::size_t bytes, int across, int channel)
{
    if (into == nullptr || across <= 0 || channel < 0 || channel >= across)
        return false;
    const auto step = static_cast<std::size_t>(across);
    const auto within = std::min(frames, bytes / (step * sizeof(float)));
    for (std::size_t i = 0; i < within; ++i)
        into[i * step + static_cast<std::size_t>(channel)] = from[i];
    return true;
}
}
