#include "ChannelOrder.h"
#include "FileStream.h"
#include "Formats.h"

#include <opus_defines.h>
#include <opusenc.h>
#include <opusfile.h>

#include <algorithm>
#include <cstdio>

// Opus through Xiph.Org's opusfile, which reads at 48 kHz as every Opus file plays, seeking to the sample and leaving out
// the encoder's start-up samples, and libopusenc, which writes from any rate at the bit rate given. Up to two channels
// are kept as they come and up to eight in Vorbis' order, as Opus' channel mapping 1 has them. Past eight there is no
// mapping opusfile reads, so none is written.
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

int readCallback(void* source, unsigned char* bytes, int count)
{
    auto& stream = *static_cast<FileStream*>(source);
    const auto got = stream.read(bytes, static_cast<std::size_t>(std::max(count, 0)));
    return stream.error ? -1 : static_cast<int>(got);
}

int seekCallback(void* source, opus_int64 offset, int whence)
{
    auto& stream = *static_cast<FileStream*>(source);
    const auto base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? static_cast<opus_int64>(stream.position)
                                                                  : static_cast<opus_int64>(stream.size);
    if (base + offset < 0)
        return -1;
    stream.position = static_cast<std::uint64_t>(base + offset);
    return 0;
}

opus_int64 tellCallback(void* source)
{
    return static_cast<opus_int64>(static_cast<FileStream*>(source)->position);
}

class OpusReader final : public AudioFileReader
{
public:
    OpusReader(std::unique_ptr<RandomAccessFile> f, const std::filesystem::path& p) : file(std::move(f))
    {
        path = p;
        information.format = "Opus";
        stream.file = file.get();
    }

    ~OpusReader() override
    {
        if (opus != nullptr)
            op_free(opus);
    }

    void start(std::error_code& error)
    {
        stream.size = file->size(error);
        if (error)
            return;

        const OpusFileCallbacks callbacks{&readCallback, &seekCallback, &tellCallback, nullptr};
        int failure = 0;
        opus = op_open_callbacks(&stream, &callbacks, nullptr, 0, &failure);
        const auto total = opus != nullptr ? op_pcm_total(opus, -1) : -1;
        if (opus == nullptr || total < 0)
        {
            error = stream.error ? stream.error : make_error_code(AudioFileError::damaged);
            return;
        }

        information.sampleRate = 48000.0;
        information.channels = op_channel_count(opus, -1);
        information.frames = static_cast<std::uint64_t>(total);

        if (const auto* tags = op_tags(opus, -1))
            for (int i = 0; i < tags->comments; ++i)
                information.tags.push_back(splitComment(tags->user_comments[i], tags->comment_lengths[i]));
    }

private:
    std::size_t readFrames(std::uint64_t from, float* const* channels, std::size_t frames, std::error_code& error) override
    {
        if (from != next)
        {
            // Opus' decoder takes a while to settle after a jump, longer than the 80 ms opusfile gives it, so it starts
            // further back and what it decodes on the way is let go.
            const auto start = from > settle ? from - settle : 0;
            if (op_pcm_seek(opus, static_cast<ogg_int64_t>(start)) != 0)
            {
                next = nowhere;
                error = stream.error ? stream.error : make_error_code(AudioFileError::damaged);
                return 0;
            }
            decodedFrames = decodedAt = 0;
            for (auto skip = from - start; skip > 0;)
            {
                if (decodedAt == decodedFrames && !decodeMore())
                    break;
                const auto now = std::min<std::uint64_t>(skip, decodedFrames - decodedAt);
                decodedAt += static_cast<std::size_t>(now);
                skip -= now;
            }
            next = from;
        }

        const auto count = information.channels;

        std::size_t done = 0;
        while (done < frames)
        {
            if (decodedAt == decodedFrames && !decodeMore())
                break;

            // A later link of a chained file may have other channels, of which those it shares are kept.
            const auto now = std::min(frames - done, decodedFrames - decodedAt);
            for (int c = 0; c < count; ++c)
            {
                const auto source = decodedChannels == count ? vorbisChannelFor(count, c) : c;
                for (std::size_t i = 0; i < now; ++i)
                    channels[c][done + i] = source < decodedChannels
                                                ? decoded[(decodedAt + i) * static_cast<std::size_t>(decodedChannels) +
                                                          static_cast<std::size_t>(source)]
                                                : 0.0f;
            }
            decodedAt += now;
            done += now;
        }

        next = from + done;
        if (stream.error)
            error = stream.error;
        return done;
    }

    // Decodes what comes next into a buffer of its own, with room for the longest a packet can be in the most channels
    // opusfile reads, as it gives as many frames as fit however many channels the link it reaches has.
    bool decodeMore()
    {
        constexpr std::size_t longest = 5760, mostItReads = 8;
        decoded.resize(longest * mostItReads);
        decodedFrames = decodedAt = 0;

        for (;;)
        {
            int link = 0;
            const auto got = op_read_float(opus, decoded.data(), static_cast<int>(decoded.size()), &link);
            if (got == OP_HOLE)
                continue;
            if (got <= 0)
                return false;

            decodedFrames = static_cast<std::size_t>(got);
            decodedChannels = op_channel_count(opus, link);
            return true;
        }
    }

    std::unique_ptr<RandomAccessFile> file;
    FileStream stream;
    OggOpusFile* opus = nullptr;

    // What was decoded and hasn't been read yet, in the channels of the link it came from.
    std::vector<float> decoded;
    std::size_t decodedFrames = 0, decodedAt = 0;
    int decodedChannels = 0;

    // Half a second, after which a stretch reads exactly as it does when the file is read through.
    static constexpr std::uint64_t settle = 24000;
    std::uint64_t next = 0;
};

class OpusWriter final : public AudioFileWriter
{
public:
    OpusWriter(std::unique_ptr<RandomAccessFile> f, const std::filesystem::path& p, const AudioFileSettings& s) : file(std::move(f))
    {
        written = s;
        path = p;
        stream.file = file.get();
    }

    ~OpusWriter() override
    {
        std::error_code ignored;
        finish(ignored);
        if (encoder != nullptr)
            ope_encoder_destroy(encoder);
        if (comments != nullptr)
            ope_comments_destroy(comments);
    }

    void start(std::error_code& error)
    {
        comments = ope_comments_create();
        if (comments == nullptr)
        {
            error = std::make_error_code(std::errc::not_enough_memory);
            finished = true;
            return;
        }

        for (const auto& [name, value] : written.tags)
            ope_comments_add(comments, name.c_str(), value.c_str());

        const OpusEncCallbacks callbacks{&writeCallback, &closeCallback};
        const auto family = written.channels <= 2 ? 0 : 1;
        int failure = 0;
        encoder = ope_encoder_create_callbacks(&callbacks, &stream, comments, static_cast<opus_int32>(written.sampleRate),
                                               written.channels, family, &failure);
        if (encoder == nullptr || ope_encoder_ctl(encoder, OPUS_SET_BITRATE(std::max(written.bitRate, 500))) != OPE_OK)
        {
            error = stream.error ? stream.error : make_error_code(AudioFileError::badSettings);
            finished = true;
        }
    }

private:
    static int writeCallback(void* source, const unsigned char* bytes, opus_int32 count)
    {
        return static_cast<FileStream*>(source)->write(bytes, static_cast<std::size_t>(count)) ? 0 : 1;
    }

    static int closeCallback(void*) { return 0; }

    void writeFrames(const float* const* channels, std::size_t frames, std::error_code& error) override
    {
        const auto stride = static_cast<std::size_t>(written.channels);
        for (std::size_t done = 0; done < frames;)
        {
            const auto now = std::min<std::size_t>(frames - done, 4096);
            interleaved.resize(now * stride);
            for (int c = 0; c < written.channels; ++c)
            {
                const auto at = static_cast<std::size_t>(vorbisChannelFor(written.channels, c));
                for (std::size_t i = 0; i < now; ++i)
                    interleaved[i * stride + at] = channels[c][done + i];
            }

            if (ope_encoder_write_float(encoder, interleaved.data(), static_cast<int>(now)) != OPE_OK)
            {
                error = stream.error ? stream.error : std::make_error_code(std::errc::io_error);
                return;
            }
            done += now;
        }
    }

    void finishFile(std::error_code& error) override
    {
        if (ope_encoder_drain(encoder) != OPE_OK && !stream.error)
            error = std::make_error_code(std::errc::io_error);
        if (stream.error)
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
    OggOpusComments* comments = nullptr;
    OggOpusEnc* encoder = nullptr;
    std::vector<float> interleaved;
};
}

std::unique_ptr<AudioFileReader> openOpus(std::unique_ptr<RandomAccessFile> file, const std::filesystem::path& path,
                                          std::error_code& error)
{
    auto reader = std::make_unique<OpusReader>(std::move(file), path);
    reader->start(error);
    if (error)
        return nullptr;
    return reader;
}

std::unique_ptr<AudioFileWriter> createOpus(std::unique_ptr<RandomAccessFile> file, const std::filesystem::path& path,
                                            const AudioFileSettings& settings, std::error_code& error)
{
    auto writer = std::make_unique<OpusWriter>(std::move(file), path, settings);
    writer->start(error);
    if (error)
        return nullptr;
    return writer;
}

}
