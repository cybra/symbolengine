#include "FileStream.h"
#include "Formats.h"

#define MINIMP3_FLOAT_OUTPUT
#include <minimp3_ex.h>

#include <algorithm>

// MP3, read through minimp3, which seeks to the sample. A file whose first frame holds LAME's or Xing's information
// reads without the encoder's delay, the decoder's 529 samples and the padding at its end, as gapless players read it,
// so the audio lies where it lay before it was encoded; a file without one reads as it decodes.
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

    ~Mp3Reader() override
    {
        if (opened)
            mp3dec_ex_close(&decoder);
    }

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
        opened = true;

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
    bool opened = false;
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
