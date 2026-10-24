#include "Mp4.h"

#include "Bytes.h"

#include <cmath>
#include <cstring>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

// MPEG-4 is boxes, each its length, a type of four letters and its body, some holding boxes of their own. A movie's
// boxes are read from the file into memory together, up to 64 MB, which holds the tables of hours of audio.
namespace symbolengine::detail
{

namespace
{
constexpr std::uint64_t largestMovie = 64 * 1024 * 1024;

struct Box
{
    std::string_view type;
    const unsigned char* body = nullptr;
    std::size_t size = 0;
};

// Each box in a stretch of bytes in turn, stopping at one that says it runs past the stretch.
template <typename Visit>
void eachBox(const unsigned char* data, std::size_t size, Visit&& visit)
{
    std::size_t at = 0;
    while (size - at >= 8)
    {
        std::uint64_t length = readBig32(data + at);
        std::size_t header = 8;
        if (length == 1)
        {
            if (size - at < 16)
                return;
            length = readBig64(data + at + 8);
            header = 16;
        }
        else if (length == 0)
        {
            length = size - at;
        }
        if (length < header || length > size - at)
            return;

        visit(Box{{reinterpret_cast<const char*>(data + at + 4), 4}, data + at + header, static_cast<std::size_t>(length) - header});
        at += static_cast<std::size_t>(length);
    }
}

std::optional<Box> find(const unsigned char* data, std::size_t size, std::string_view type)
{
    std::optional<Box> found;
    eachBox(data, size, [&](const Box& box) {
        if (!found && box.type == type)
            found = box;
    });
    return found;
}

std::optional<Box> find(const std::optional<Box>& in, std::string_view type)
{
    return in ? find(in->body, in->size, type) : std::nullopt;
}

std::string_view text(const unsigned char* at, std::size_t size)
{
    return {reinterpret_cast<const char*>(at), size};
}

// A time scale, which a header box keeps after its version, its flags and two times of 32 bits each in version 0
// and 64 in version 1.
std::uint32_t scaleOf(const std::optional<Box>& header)
{
    if (!header || header->size < 4)
        return 0;
    const std::size_t at = header->body[0] == 1 ? 20 : 12;
    return header->size >= at + 4 ? readBig32(header->body + at) : 0;
}

std::uint64_t framesFor(double units, std::uint32_t scale, double rate)
{
    return static_cast<std::uint64_t>(std::llround(units * rate / scale));
}

// iTunes' gapless information: words in hexadecimal, the second the priming, the third the padding and the fourth
// the audio's length.
void readGapless(const unsigned char* moov, std::size_t size, Mp4Audio& audio)
{
    const auto meta = find(find(moov, size, "udta"), "meta");
    if (!meta || meta->size < 8)
        return;

    // A meta box has a version and flags first, except in some files that leave them out.
    const std::size_t skip = text(meta->body + 4, 4) == "hdlr" ? 0 : 4;
    const auto items = find(meta->body + skip, meta->size - skip, "ilst");
    if (!items)
        return;

    eachBox(items->body, items->size, [&](const Box& item) {
        if (item.type != "----")
            return;
        const auto name = find(item.body, item.size, "name");
        const auto data = find(item.body, item.size, "data");
        if (!name || name->size < 4 || text(name->body + 4, name->size - 4) != "iTunSMPB" || !data || data->size < 8)
            return;

        std::istringstream words{std::string(text(data->body + 8, data->size - 8))};
        std::vector<std::uint64_t> values;
        for (std::uint64_t value = 0; words >> std::hex >> value;)
            values.push_back(value);
        if (values.size() >= 4 && values[3] > 0)
        {
            audio.priming = values[1];
            audio.frames = values[3];
        }
    });
}

Mp4Audio readMovie(const unsigned char* moov, std::size_t size, double rate)
{
    Mp4Audio audio;
    const auto movieScale = scaleOf(find(moov, size, "mvhd"));
    auto found = false;

    eachBox(moov, size, [&](const Box& track) {
        if (found || track.type != "trak")
            return;
        const auto media = find(track.body, track.size, "mdia");
        const auto handler = find(media, "hdlr");
        if (!handler || handler->size < 12 || text(handler->body + 8, 4) != "soun")
            return;
        found = true;

        // The first edit that plays the media, after any empty one, says where the audio starts and how long it lasts.
        const auto mediaScale = scaleOf(find(media, "mdhd"));
        if (const auto edits = find(find(track.body, track.size, "edts"), "elst"); edits && edits->size >= 8)
        {
            const auto wide = edits->body[0] == 1;
            const std::size_t entry = wide ? 20 : 12;
            const auto count = readBig32(edits->body + 4);
            for (std::size_t i = 0; i < count && 8 + (i + 1) * entry <= edits->size; ++i)
            {
                const auto* at = edits->body + 8 + i * entry;
                const auto duration = wide ? readBig64(at) : readBig32(at);
                const auto start = wide ? static_cast<std::int64_t>(readBig64(at + 8)) : static_cast<std::int32_t>(readBig32(at + 4));
                if (start < 0)
                    continue;
                if (mediaScale > 0)
                    audio.priming = framesFor(static_cast<double>(start), mediaScale, rate);
                if (movieScale > 0)
                    audio.frames = framesFor(static_cast<double>(duration), movieScale, rate);
                audio.edited = true;
                break;
            }
        }

        // Apple Lossless keeps its bits in its own box, after an audio description's 28 bytes of fields, which follow
        // the description table's version, flags and count.
        const auto descriptions = find(find(find(media, "minf"), "stbl"), "stsd");
        if (!descriptions || descriptions->size < 8)
            return;
        if (const auto first = find(descriptions->body + 8, descriptions->size - 8, "alac"); first && first->size >= 28)
            if (const auto config = find(first->body + 28, first->size - 28, "alac"); config && config->size >= 10)
                audio.losslessBits = config->body[9];
    });

    if (!audio.edited)
        readGapless(moov, size, audio);
    return audio;
}
}

Mp4Audio readMp4Audio(const RandomAccessFile& file, double sampleRate, std::error_code& error)
{
    const auto total = file.size(error);
    if (error)
        return {};

    for (std::uint64_t at = 0; total - at >= 8;)
    {
        unsigned char header[16]{};
        const auto got = file.readAt(at, header, sizeof header, error);
        if (error || got < 8)
            return {};

        std::uint64_t length = readBig32(header);
        std::uint64_t headerSize = 8;
        if (length == 1)
        {
            if (got < 16)
                return {};
            length = readBig64(header + 8);
            headerSize = 16;
        }
        else if (length == 0)
        {
            length = total - at;
        }
        if (length < headerSize || length > total - at)
            return {};

        if (std::memcmp(header + 4, "moov", 4) == 0)
        {
            if (length - headerSize > largestMovie)
                return {};
            std::string movie(static_cast<std::size_t>(length - headerSize), '\0');
            if (file.readAt(at + headerSize, movie.data(), movie.size(), error) != movie.size() || error)
                return {};
            return readMovie(bytesOf(movie), movie.size(), sampleRate);
        }
        at += length;
    }
    return {};
}

}
