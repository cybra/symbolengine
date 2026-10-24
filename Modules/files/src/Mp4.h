#pragma once

#include "RandomAccessFile.h"

#include <cstdint>
#include <optional>
#include <system_error>

namespace symbolengine::detail
{

/** What an MPEG-4 file says of its first audio track that not every decoder acts on: how many samples its encoder put
    before the audio and how many are audio, from the track's edit list or else iTunes' iTunSMPB, and the bits an
    Apple Lossless track keeps. */
struct Mp4Audio
{
    std::uint64_t priming = 0;
    std::optional<std::uint64_t> frames;
    int losslessBits = 0;
};

/** Reads that from a file whose samples play at the rate given; for a file it can't make out, what it found, which may
    be nothing, with the error set only when the file couldn't be read. */
Mp4Audio readMp4Audio(const RandomAccessFile& file, double sampleRate, std::error_code& error);

}
