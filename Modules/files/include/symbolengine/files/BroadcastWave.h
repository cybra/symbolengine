#pragma once

#include "AudioFile.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace symbolengine
{

/** A WAV's broadcast extension, its bext chunk as EBU Tech 3285 describes it: what the recording is, who made it and
    when, and where it starts in the day, which recorders and editors line recordings up by. Text is kept as the file
    has it, each field cut at its size when written. */
struct BroadcastExtension
{
    std::string description{};            // up to 256 bytes
    std::string originator{};             // up to 32
    std::string originatorReference{};    // up to 32
    std::string originationDate{};        // yyyy-mm-dd, 10
    std::string originationTime{};        // hh:mm:ss, 8
    std::uint64_t timeReference = 0;      // the first sample's place in the day, in samples since midnight
    std::uint16_t version = 1;
    std::string umid{};                   // the recording's SMPTE UMID, up to 64 bytes, or nothing
    std::string codingHistory{};          // what the audio has been through, a line for each step

    bool operator==(const BroadcastExtension&) const = default;
};

namespace files
{

/** The broadcast extension in a WAV's chunks, as AudioInfo gives them, or nothing when it has none. */
std::optional<BroadcastExtension> broadcastExtension(const std::vector<AudioChunk>& chunks);

/** A bext chunk holding the extension, to give AudioFileSettings. A version 2 extension's loudness fields are
    written as not measured. */
AudioChunk bextChunk(const BroadcastExtension& extension);

}

}
