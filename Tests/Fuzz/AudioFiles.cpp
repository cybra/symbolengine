// Any bytes as an audio file: it opens or says why it can't, and what opens reads within the room given, from anywhere,
// and stops, whatever its headers and streams say.

#include "Fuzz.h"

#include <symbolengine/files/AudioFile.h>

#include <algorithm>
#include <cstring>
#include <string_view>
#include <vector>

using namespace symbolengine;

namespace
{
constexpr std::size_t piece = 256;

// Room past what each read asks for, which must stay as it was.
constexpr std::size_t past = 16384;
constexpr float untouched = 1234.5f;

// As much as is worth reading of one input.
constexpr std::uint64_t mostFrames = 1u << 17;

#if defined(__APPLE__)
// The Mac's own decoders are Apple's to fuzz, and all but the formats below reach them.
bool ours(const std::uint8_t* data, std::size_t size)
{
    const auto is = [&](std::string_view id) { return size >= id.size() && std::memcmp(data, id.data(), id.size()) == 0; };
    return is("RIFF") || is("RF64") || is("BW64") || is("FORM") || is("fLaC") || is("OggS") || is("ID3") ||
           (size >= 2 && data[0] == 0xFF && (data[1] & 0xE0) == 0xE0);
}
#endif
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
#if defined(__APPLE__)
    if (!ours(data, size))
        return 0;
#endif

    static const auto file = fuzz::scratch("audio");
    fuzz::write(file, data, size);

    std::error_code error;
    const auto reader = files::openAudio(file, error);
    if (reader == nullptr)
    {
        if (!error)
            fuzz::broken();
        return 0;
    }

    const auto& info = reader->info();
    if (info.channels < 1 || !(info.sampleRate > 0.0) || info.sampleRate > 4294967295.0)
        fuzz::broken();

    std::vector<std::vector<float>> room(static_cast<std::size_t>(info.channels), std::vector<float>(piece + past, untouched));
    std::vector<float*> pointers;
    for (auto& channel : room)
        pointers.push_back(channel.data());

    const auto readFrom = [&](std::uint64_t start, std::uint64_t most) {
        std::uint64_t at = start;
        while (at < start + most)
        {
            std::error_code failed;
            const auto got = reader->read(at, pointers.data(), piece, failed);
            if (got > piece)
                fuzz::broken();
            for (const auto& channel : room)
                if (!std::all_of(channel.begin() + static_cast<std::ptrdiff_t>(piece), channel.end(),
                                 [](float sample) { return sample == untouched; }))
                    fuzz::broken();
            if (got == 0 || failed)
                break;
            at += got;
        }
    };

    readFrom(0, mostFrames);
    readFrom(info.frames / 2, piece * 4);
    readFrom(info.frames > piece ? info.frames - piece : 0, piece * 2);
    readFrom(0, piece * 2);

    // Past its end there is nothing, however long it says it is.
    if (reader->read(info.frames, pointers.data(), piece, error) != 0)
        fuzz::broken();

    return 0;
}
