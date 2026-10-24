#include "Bytes.h"
#include "FileStream.h"
#include "Formats.h"

#define MINIMP3_FLOAT_OUTPUT
#include <minimp3_ex.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

// MP3, read through minimp3, which seeks to the sample. A file whose first frame holds LAME's information, as LAME and
// ffmpeg write it, reads without the encoder's delay, the decoder's 529 samples and the padding at its end, as gapless
// players read it, so the audio lies where it lay before it was encoded; a file without it reads as it decodes, and so
// does one whose information another encoder wrote, which may keep other things where LAME keeps the delay.
namespace symbolengine::detail
{

namespace
{
std::size_t readCallback(void* bytes, std::size_t count, void* source)
{
    return static_cast<FileStream*>(source)->read(bytes, count);
}

int seekCallback(std::uint64_t position, void* source)
{
    static_cast<FileStream*>(source)->position = position;
    return 0;
}

// The information in an MP3's first frame: past an ID3 tag and any stray bytes, a layer III frame's header and side
// information, then "Xing" or "Info", its flags and the fields they say are there, and LAME's extension, whose first
// four bytes name the encoder.
struct Information
{
    bool found = false;
    std::string encoder;
    std::uint64_t frames = 0;
    std::uint64_t samplesPerFrame = 1152;
};

Information readInformation(const RandomAccessFile& file, std::error_code& error)
{
    unsigned char start[10]{};
    std::uint64_t at = 0;
    if (file.readAt(0, start, sizeof start, error) == sizeof start && std::memcmp(start, "ID3", 3) == 0)
        at = 10 + (static_cast<std::uint64_t>(start[6] & 0x7F) << 21 | static_cast<std::uint64_t>(start[7] & 0x7F) << 14 |
                   static_cast<std::uint64_t>(start[8] & 0x7F) << 7 | static_cast<std::uint64_t>(start[9] & 0x7F)) +
             ((start[5] & 0x10) != 0 ? 10 : 0);

    std::vector<unsigned char> bytes(4096);
    bytes.resize(error ? 0 : file.readAt(at, bytes.data(), bytes.size(), error));
    if (error)
        return {};

    for (std::size_t i = 0; i + 4 <= bytes.size(); ++i)
    {
        const auto* p = bytes.data() + i;
        const auto version = (p[1] >> 3) & 3, layer = (p[1] >> 1) & 3, bitRate = p[2] >> 4, rate = (p[2] >> 2) & 3;
        if (p[0] != 0xFF || (p[1] & 0xE0) != 0xE0 || version == 1 || layer != 1 || bitRate == 15 || rate == 3)
            continue;

        const auto first = version == 3;
        const auto mono = (p[3] >> 6) == 3;
        auto tag = i + 4 + ((p[1] & 1) == 0 ? 2u : 0u) + (first ? (mono ? 17u : 32u) : (mono ? 9u : 17u));
        if (tag + 8 > bytes.size() || (std::memcmp(bytes.data() + tag, "Xing", 4) != 0 && std::memcmp(bytes.data() + tag, "Info", 4) != 0))
            return {};

        Information told;
        told.found = true;
        told.samplesPerFrame = first ? 1152 : 576;
        const auto flags = readBig32(bytes.data() + tag + 4);
        auto field = tag + 8;
        if ((flags & 1) != 0 && field + 4 <= bytes.size())
        {
            told.frames = readBig32(bytes.data() + field);
            field += 4;
        }
        field += ((flags & 2) != 0 ? 4u : 0u) + ((flags & 4) != 0 ? 100u : 0u) + ((flags & 8) != 0 ? 4u : 0u);
        if (field + 4 <= bytes.size())
            told.encoder.assign(reinterpret_cast<const char*>(bytes.data() + field), 4);
        return told;
    }
    return {};
}

class Mp3Reader final : public AudioFileReader
{
public:
    Mp3Reader(std::unique_ptr<RandomAccessFile> f, const std::filesystem::path& p) : file(std::move(f))
    {
        path = p;
        stream.file = file.get();
        io.read = &readCallback;
        io.read_data = &stream;
        io.seek = &seekCallback;
        io.seek_data = &stream;
    }

    // Closed whether or not it opened, as a file that failed to open still holds what minimp3 took for reading it.
    ~Mp3Reader() override { mp3dec_ex_close(&decoder); }

    void start(std::error_code& error)
    {
        stream.size = file->size(error);
        if (error)
            return;

        if (mp3dec_ex_open_cb(&decoder, &io, MP3D_SEEK_TO_SAMPLE) != 0 || decoder.info.channels <= 0 || decoder.info.hz <= 0)
        {
            error = stream.error ? stream.error : make_error_code(AudioFileError::notAudio);
            return;
        }

        // minimp3 leaves out the delay and padding whatever encoder's information gives them; another encoder's are
        // put back, with the length its frames make.
        if (decoder.vbr_tag_found)
        {
            const auto told = readInformation(*file, error);
            if (error)
                return;
            if (told.found && told.frames > 0 && told.encoder != "LAME" && told.encoder != "Lavf" && told.encoder != "Lavc")
            {
                decoder.start_delay = decoder.to_skip = 0;
                decoder.samples = decoder.detected_samples =
                    told.frames * told.samplesPerFrame * static_cast<std::uint64_t>(decoder.info.channels);
            }
        }

        information.format = decoder.info.layer == 1 ? "MP1" : decoder.info.layer == 2 ? "MP2" : "MP3";
        information.sampleRate = decoder.info.hz;
        information.channels = decoder.info.channels;
        information.frames = decoder.samples / static_cast<std::uint64_t>(decoder.info.channels);
    }

private:
    std::size_t readFrames(std::uint64_t from, float* const* channels, std::size_t frames, std::error_code& error) override
    {
        const auto count = static_cast<std::size_t>(information.channels);

        if (from != next)
        {
            if (mp3dec_ex_seek(&decoder, from * count) != 0)
            {
                error = stream.error ? stream.error : make_error_code(AudioFileError::damaged);
                return 0;
            }
            next = from;
        }

        std::size_t done = 0;
        while (done < frames)
        {
            decoded.resize(std::min<std::size_t>(frames - done, 4608) * count);
            const auto got = mp3dec_ex_read(&decoder, decoded.data(), decoded.size()) / count;
            for (std::size_t c = 0; c < count; ++c)
                for (std::size_t i = 0; i < got; ++i)
                    channels[c][done + i] = decoded[i * count + c];
            done += got;

            if (got * count < decoded.size())
                break;
        }

        next = from + done;
        if (stream.error)
            error = stream.error;
        return done;
    }

    std::unique_ptr<RandomAccessFile> file;
    FileStream stream;
    mp3dec_io_t io{};
    mp3dec_ex_t decoder{};
    std::vector<float> decoded;
    std::uint64_t next = 0;
};
}

std::unique_ptr<AudioFileReader> openMp3(std::unique_ptr<RandomAccessFile> file, const std::filesystem::path& path,
                                         std::error_code& error)
{
    auto reader = std::make_unique<Mp3Reader>(std::move(file), path);
    reader->start(error);
    if (error)
        return nullptr;
    return reader;
}

}
