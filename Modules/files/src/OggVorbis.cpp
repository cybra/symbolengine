#include "ChannelOrder.h"
#include "FileStream.h"
#include "Formats.h"

#include <vorbis/vorbisenc.h>
#include <vorbis/vorbisfile.h>

#include <algorithm>
#include <cstdio>
#include <random>

// Ogg Vorbis through Xiph.Org's libvorbis: read with vorbisfile, which seeks to the sample, and written with vorbisenc
// at the quality given, its packets laid into Ogg pages by libogg.
namespace symbolengine::detail
{

namespace
{
std::pair<std::string, std::string> splitComment(const char* entry, int length)
{
    const std::string text(entry, static_cast<std::size_t>(std::max(length, 0)));
    const auto equals = text.find('=');
    return equals == std::string::npos ? std::pair{text, std::string()} : std::pair{text.substr(0, equals), text.substr(equals + 1)};
}

std::size_t readCallback(void* bytes, std::size_t size, std::size_t count, void* source)
{
    auto& stream = *static_cast<FileStream*>(source);
    return size == 0 ? 0 : stream.read(bytes, size * count) / size;
}

int seekCallback(void* source, ogg_int64_t offset, int whence)
{
    auto& stream = *static_cast<FileStream*>(source);
    const auto base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? static_cast<ogg_int64_t>(stream.position)
                                                                  : static_cast<ogg_int64_t>(stream.size);
    if (base + offset < 0)
        return -1;
    stream.position = static_cast<std::uint64_t>(base + offset);
    return 0;
}

long tellCallback(void* source)
{
    return static_cast<long>(static_cast<FileStream*>(source)->position);
}

class VorbisReader final : public AudioFileReader
{
public:
    VorbisReader(std::unique_ptr<RandomAccessFile> f, const std::filesystem::path& p) : file(std::move(f))
    {
        path = p;
        information.format = "Ogg Vorbis";
        stream.file = file.get();
    }

    ~VorbisReader() override
    {
        if (opened)
            ov_clear(&vorbis);
    }

    void start(std::error_code& error)
    {
        stream.size = file->size(error);
        if (error)
            return;

        const ov_callbacks callbacks{&readCallback, &seekCallback, nullptr, &tellCallback};
        if (ov_open_callbacks(&stream, &vorbis, nullptr, 0, callbacks) != 0)
        {
            error = stream.error ? stream.error : make_error_code(AudioFileError::damaged);
            return;
        }
        opened = true;

        const auto* info = ov_info(&vorbis, -1);
        const auto total = ov_pcm_total(&vorbis, -1);
        if (info == nullptr || info->channels <= 0 || total < 0)
        {
            error = make_error_code(AudioFileError::damaged);
            return;
        }

        information.sampleRate = static_cast<double>(info->rate);
        information.channels = info->channels;

        // Links at another rate would play at the wrong speed in one file's samples, so it ends where its rate does.
        std::uint64_t held = 0;
        for (int link = 0, links = static_cast<int>(ov_streams(&vorbis)); link < links; ++link)
        {
            const auto* const linked = ov_info(&vorbis, link);
            if (linked == nullptr || linked->rate != info->rate)
                break;
            held += static_cast<std::uint64_t>(std::max<ogg_int64_t>(ov_pcm_total(&vorbis, link), 0));
        }
        information.frames = held;

        if (const auto* comments = ov_comment(&vorbis, -1))
            for (int i = 0; i < comments->comments; ++i)
                information.tags.push_back(splitComment(comments->user_comments[i], comments->comment_lengths[i]));
    }

private:
    std::size_t readFrames(std::uint64_t from, float* const* channels, std::size_t frames, std::error_code& error) override
    {
        if (from != next)
        {
            if (ov_pcm_seek(&vorbis, static_cast<ogg_int64_t>(from)) != 0)
            {
                next = nowhere;
                error = stream.error ? stream.error : make_error_code(AudioFileError::damaged);
                return 0;
            }
            next = from;
        }

        std::size_t done = 0;
        while (done < frames)
        {
            float** decoded = nullptr;
            int section = 0;
            const auto got = ov_read_float(&vorbis, &decoded, static_cast<int>(std::min<std::size_t>(frames - done, 4096)), &section);
            if (got == OV_HOLE)
                continue;
            if (got <= 0)
                break;

            // A later link of a chained file may have other channels, of which those it shares are kept.
            const auto* info = ov_info(&vorbis, section);
            const auto have = info != nullptr ? info->channels : information.channels;
            for (int c = 0; c < information.channels; ++c)
            {
                const auto source = have == information.channels ? vorbisChannelFor(have, c) : c;
                for (long i = 0; i < got; ++i)
                    channels[c][done + static_cast<std::size_t>(i)] = source < have ? decoded[source][i] : 0.0f;
            }
            done += static_cast<std::size_t>(got);
        }

        next = from + done;
        if (stream.error)
            error = stream.error;
        return done;
    }

    std::unique_ptr<RandomAccessFile> file;
    FileStream stream;
    OggVorbis_File vorbis{};
    bool opened = false;
    std::uint64_t next = 0;
};

class VorbisWriter final : public AudioFileWriter
{
public:
    VorbisWriter(std::unique_ptr<RandomAccessFile> f, const std::filesystem::path& p, const AudioFileSettings& s) : file(std::move(f))
    {
        written = s;
        path = p;
        stream.file = file.get();
    }

    ~VorbisWriter() override
    {
        std::error_code ignored;
        finish(ignored);
        if (started)
        {
            ogg_stream_clear(&ogg);
            vorbis_block_clear(&block);
            vorbis_dsp_clear(&dsp);
        }
        vorbis_comment_clear(&comments);
        vorbis_info_clear(&info);
    }

    void start(std::error_code& error)
    {
        vorbis_info_init(&info);
        vorbis_comment_init(&comments);

        if (vorbis_encode_init_vbr(&info, written.channels, static_cast<long>(written.sampleRate),
                                   static_cast<float>(std::clamp(written.quality, -0.1, 1.0))) != 0)
        {
            error = AudioFileError::badSettings;
            finished = true;
            return;
        }

        for (const auto& [name, value] : written.tags)
            vorbis_comment_add_tag(&comments, name.c_str(), value.c_str());

        vorbis_analysis_init(&dsp, &info);
        vorbis_block_init(&dsp, &block);
        std::random_device random;
        ogg_stream_init(&ogg, static_cast<int>(random() & 0x7FFFFFFF));
        started = true;

        ogg_packet identification, commentHeader, codebooks;
        vorbis_analysis_headerout(&dsp, &comments, &identification, &commentHeader, &codebooks);
        ogg_stream_packetin(&ogg, &identification);
        ogg_stream_packetin(&ogg, &commentHeader);
        ogg_stream_packetin(&ogg, &codebooks);

        // The headers end a page of their own, so the audio starts on a page of its own as the format asks.
        ogg_page page;
        while (ogg_stream_flush(&ogg, &page) != 0)
            writePage(page);

        if (stream.error)
        {
            error = stream.error;
            finished = true;
        }
    }

private:
    void writePage(const ogg_page& page)
    {
        stream.write(page.header, static_cast<std::size_t>(page.header_len));
        stream.write(page.body, static_cast<std::size_t>(page.body_len));
    }

    // Encodes whatever the encoder has ready and writes the pages it fills.
    void drain()
    {
        while (vorbis_analysis_blockout(&dsp, &block) == 1)
        {
            vorbis_analysis(&block, nullptr);
            vorbis_bitrate_addblock(&block);

            ogg_packet packet;
            while (vorbis_bitrate_flushpacket(&dsp, &packet) != 0)
            {
                ogg_stream_packetin(&ogg, &packet);
                ogg_page page;
                while (ogg_stream_pageout(&ogg, &page) != 0)
                    writePage(page);
            }
        }
    }

    void writeFrames(const float* const* channels, std::size_t frames, std::error_code& error) override
    {
        for (std::size_t done = 0; done < frames;)
        {
            const auto now = std::min<std::size_t>(frames - done, 4096);
            auto** buffer = vorbis_analysis_buffer(&dsp, static_cast<int>(now));
            for (int c = 0; c < written.channels; ++c)
                std::copy_n(channels[c] + done, now, buffer[vorbisChannelFor(written.channels, c)]);
            vorbis_analysis_wrote(&dsp, static_cast<int>(now));
            drain();
            done += now;
        }

        if (stream.error)
            error = stream.error;
    }

    void finishFile(std::error_code& error) override
    {
        vorbis_analysis_wrote(&dsp, 0);
        drain();
        ogg_page page;
        while (ogg_stream_flush(&ogg, &page) != 0)
            writePage(page);

        error = stream.error;
        if (!error)
            file->sync(error);

        std::error_code closing;
        file->close(closing);
        if (!error)
            error = closing;
    }

    std::unique_ptr<RandomAccessFile> file;
    FileStream stream;
    vorbis_info info{};
    vorbis_comment comments{};
    vorbis_dsp_state dsp{};
    vorbis_block block{};
    ogg_stream_state ogg{};
    bool started = false;
};
}

std::unique_ptr<AudioFileReader> openVorbis(std::unique_ptr<RandomAccessFile> file, const std::filesystem::path& path,
                                            std::error_code& error)
{
    auto reader = std::make_unique<VorbisReader>(std::move(file), path);
    reader->start(error);
    if (error)
        return nullptr;
    return reader;
}

std::unique_ptr<AudioFileWriter> createVorbis(std::unique_ptr<RandomAccessFile> file, const std::filesystem::path& path,
                                              const AudioFileSettings& settings, std::error_code& error)
{
    auto writer = std::make_unique<VorbisWriter>(std::move(file), path, settings);
    writer->start(error);
    if (error)
        return nullptr;
    return writer;
}

}
