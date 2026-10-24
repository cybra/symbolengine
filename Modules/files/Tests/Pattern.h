#pragma once

#include <symbolengine/files/AudioFile.h>

#include <cstdint>
#include <filesystem>
#include <vector>

// The samples the reference files hold, as make_reference_files.py works them out, and helpers for reading files.
namespace pattern
{

constexpr std::uint64_t referenceFrames = 1000;

/** A whole-number sample: the lowest and highest values first, then values spread over the whole range. */
inline std::int64_t whole(std::uint64_t frame, int channel, int bits)
{
    if (frame == 0)
        return -(std::int64_t{1} << (bits - 1));
    if (frame == 1)
        return (std::int64_t{1} << (bits - 1)) - 1;
    const auto mixed = ((frame * 7919 + static_cast<std::uint64_t>(channel) * 104729) * 2654435761u) % (std::uint64_t{1} << 32);
    return static_cast<std::int64_t>(mixed >> (32 - bits)) - (std::int64_t{1} << (bits - 1));
}

/** The same sample as a float file holds it, some past full scale. */
inline double floating(std::uint64_t frame, int channel)
{
    return static_cast<double>(whole(frame, channel, 24)) / 8388608.0 * 1.5;
}

/** What a whole-number sample reads as. */
inline float read(std::uint64_t frame, int channel, int bits)
{
    return static_cast<float>(static_cast<double>(whole(frame, channel, bits)) / static_cast<double>(std::int64_t{1} << (bits - 1)));
}

inline std::filesystem::path reference(const char* name)
{
    return std::filesystem::path(SYMBOLENGINE_FILES_TEST_DATA) / name;
}

/** Every sample of a file, a channel each, read in stretches of the size given. */
inline std::vector<std::vector<float>> readAll(symbolengine::AudioFileReader& reader, std::size_t stretch = 333)
{
    const auto& info = reader.info();
    std::vector<std::vector<float>> channels(static_cast<std::size_t>(info.channels),
                                             std::vector<float>(static_cast<std::size_t>(info.frames)));
    std::vector<float*> pointers(channels.size());

    for (std::uint64_t at = 0; at < info.frames;)
    {
        for (std::size_t c = 0; c < channels.size(); ++c)
            pointers[c] = channels[c].data() + at;
        const auto got = reader.read(at, pointers.data(), stretch);
        if (got == 0)
            break;
        at += got;
    }

    return channels;
}

}
