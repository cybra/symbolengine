#include "FileStream.h"
#include "Formats.h"
#include "Samples.h"

#include <FLAC/metadata.h>
#include <FLAC/stream_decoder.h>
#include <FLAC/stream_encoder.h>

#include <algorithm>
#include <cstring>

// FLAC through Xiph.Org's libFLAC: read with its stream decoder from wherever a read asks, seeking to the sample, and
// written with its encoder, which goes back to fill in the length and the MD5 signature once the samples are all in.
// Its channels are in WAV's order already, up to eight.
namespace symbolengine::detail
{

namespace
{
std::pair<std::string, std::string> splitComment(const char* entry, std::size_t length)
{
    const std::string text(entry, length);
    const auto equals = text.find('=');
    return equals == std::string::npos ? std::pair{text, std::string()} : std::pair{text.substr(0, equals), text.substr(equals + 1)};
}

class FlacReader final : public AudioFileReader
{
public:
    FlacReader(std::unique_ptr<RandomAccessFile> f, const std::filesystem::path& p) : file(std::move(f))
    {
        path = p;
        information.format = "FLAC";
        stream.file = file.get();
    }

    ~FlacReader() override
    {
        if (decoder != nullptr)
        {
            FLAC__stream_decoder_finish(decoder);
            FLAC__stream_decoder_delete(decoder);
        }
    }

    void start(std::error_code& error)
    {
        stream.size = file->size(error);
        if (error)
            return;

        decoder = FLAC__stream_decoder_new();
        if (decoder == nullptr)
        {
            error = std::make_error_code(std::errc::not_enough_memory);
            return;
        }

        FLAC__stream_decoder_set_metadata_respond(decoder, FLAC__METADATA_TYPE_VORBIS_COMMENT);
        if (FLAC__stream_decoder_init_stream(decoder, &read, &seek, &tell, &length, &endOfFile, &write, &metadata,
                                             &problem, this) != FLAC__STREAM_DECODER_INIT_STATUS_OK ||
            !FLAC__stream_decoder_process_until_end_of_metadata(decoder) || information.channels == 0 ||
            !(information.sampleRate > 0.0))
        {
            error = stream.error ? stream.error : make_error_code(AudioFileError::damaged);
            return;
        }

        // A stream that doesn't say its length is measured by decoding it once.
        if (information.frames == 0)
        {
            counting = true;
            while (FLAC__stream_decoder_get_state(decoder) != FLAC__STREAM_DECODER_END_OF_STREAM &&
                   FLAC__stream_decoder_process_single(decoder))
            {
            }
            counting = false;
            FLAC__stream_decoder_reset(decoder);
            FLAC__stream_decoder_process_until_end_of_metadata(decoder);
            information.frames = counted;
        }
    }

private:
    std::size_t readFrames(std::uint64_t from, float* const* channels, std::size_t frames, std::error_code& error) override
    {
        if (from != next)
        {
            pending.assign(pending.size(), {});
            pendingAt = 0;
            if (!FLAC__stream_decoder_seek_absolute(decoder, from))
            {
                next = nowhere;
                FLAC__stream_decoder_flush(decoder);
                error = stream.error ? stream.error : make_error_code(AudioFileError::damaged);
                return 0;
            }
            next = from;
        }

        std::size_t done = 0;
        while (done < frames)
        {
            const auto available = pending.empty() ? 0 : pending.front().size() - pendingAt;
            if (available == 0)
            {
                for (auto& channel : pending)
                    channel.clear();
                pendingAt = 0;
                if (FLAC__stream_decoder_get_state(decoder) == FLAC__STREAM_DECODER_END_OF_STREAM ||
                    !FLAC__stream_decoder_process_single(decoder) || pending.front().empty())
                    break;
                continue;
            }

            const auto now = std::min(available, frames - done);
            for (int c = 0; c < information.channels; ++c)
                std::copy_n(pending[static_cast<std::size_t>(c)].begin() + static_cast<std::ptrdiff_t>(pendingAt), now, channels[c] + done);
            pendingAt += now;
            done += now;
        }

        next = from + done;
        if (stream.error)
            error = stream.error;
        return done;
    }

    static FLAC__StreamDecoderReadStatus read(const FLAC__StreamDecoder*, FLAC__byte buffer[], size_t* bytes, void* self)
    {
        auto& stream = static_cast<FlacReader*>(self)->stream;
        *bytes = stream.read(buffer, *bytes);
        if (stream.error)
            return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
        return *bytes == 0 ? FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM : FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
    }

    static FLAC__StreamDecoderSeekStatus seek(const FLAC__StreamDecoder*, FLAC__uint64 at, void* self)
    {
        static_cast<FlacReader*>(self)->stream.position = at;
        return FLAC__STREAM_DECODER_SEEK_STATUS_OK;
    }

    static FLAC__StreamDecoderTellStatus tell(const FLAC__StreamDecoder*, FLAC__uint64* at, void* self)
    {
        *at = static_cast<FlacReader*>(self)->stream.position;
        return FLAC__STREAM_DECODER_TELL_STATUS_OK;
    }

    static FLAC__StreamDecoderLengthStatus length(const FLAC__StreamDecoder*, FLAC__uint64* size, void* self)
    {
        *size = static_cast<FlacReader*>(self)->stream.size;
        return FLAC__STREAM_DECODER_LENGTH_STATUS_OK;
    }

    static FLAC__bool endOfFile(const FLAC__StreamDecoder*, void* self)
    {
        const auto& stream = static_cast<FlacReader*>(self)->stream;
        return stream.position >= stream.size;
    }

    static FLAC__StreamDecoderWriteStatus write(const FLAC__StreamDecoder*, const FLAC__Frame* frame,
                                                const FLAC__int32* const buffer[], void* self)
    {
        auto& reader = *static_cast<FlacReader*>(self);
        const auto frames = frame->header.blocksize;

        if (reader.counting)
        {
            reader.counted += frames;
            return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
        }

        const auto scale = 1.0f / static_cast<float>(std::int64_t{1} << (reader.information.bits - 1));
        const auto channels = std::min(static_cast<int>(frame->header.channels), reader.information.channels);
        for (int c = 0; c < reader.information.channels; ++c)
        {
            auto& out = reader.pending[static_cast<std::size_t>(c)];
            out.resize(frames);
            for (std::uint32_t i = 0; i < frames; ++i)
                out[i] = c < channels ? static_cast<float>(buffer[c][i]) * scale : 0.0f;
        }
        return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
    }

    static void metadata(const FLAC__StreamDecoder*, const FLAC__StreamMetadata* block, void* self)
    {
        auto& info = static_cast<FlacReader*>(self)->information;

        if (block->type == FLAC__METADATA_TYPE_STREAMINFO)
        {
            const auto& stream = block->data.stream_info;
            info.sampleRate = stream.sample_rate;
            info.channels = static_cast<int>(stream.channels);
            info.bits = static_cast<int>(stream.bits_per_sample);
            info.frames = stream.total_samples;
            static_cast<FlacReader*>(self)->pending.assign(stream.channels, {});
        }
        else if (block->type == FLAC__METADATA_TYPE_VORBIS_COMMENT && info.tags.empty())
        {
            const auto& comments = block->data.vorbis_comment;
            for (std::uint32_t i = 0; i < comments.num_comments; ++i)
                info.tags.push_back(splitComment(reinterpret_cast<const char*>(comments.comments[i].entry), comments.comments[i].length));
        }
    }

    // A frame that fails its checks is skipped and decoding carries on, as players do.
    static void problem(const FLAC__StreamDecoder*, FLAC__StreamDecoderErrorStatus, void*) {}

    std::unique_ptr<RandomAccessFile> file;
    FileStream stream;
    FLAC__StreamDecoder* decoder = nullptr;
    std::vector<std::vector<float>> pending;
    std::size_t pendingAt = 0;
    std::uint64_t next = 0;
    bool counting = false;
    std::uint64_t counted = 0;
};

class FlacWriter final : public AudioFileWriter
{
public:
    FlacWriter(std::unique_ptr<RandomAccessFile> f, const std::filesystem::path& p, const AudioFileSettings& s) : file(std::move(f))
    {
        written = s;
        path = p;
        stream.file = file.get();
    }

    ~FlacWriter() override
    {
        std::error_code ignored;
        finish(ignored);
        if (encoder != nullptr)
            FLAC__stream_encoder_delete(encoder);
        for (auto* block : blocks)
            FLAC__metadata_object_delete(block);
    }

    void start(std::error_code& error)
    {
        encoder = FLAC__stream_encoder_new();
        auto* comments = FLAC__metadata_object_new(FLAC__METADATA_TYPE_VORBIS_COMMENT);
        auto* padding = FLAC__metadata_object_new(FLAC__METADATA_TYPE_PADDING);
        if (encoder == nullptr || comments == nullptr || padding == nullptr)
        {
            error = std::make_error_code(std::errc::not_enough_memory);
            finished = true;
            return;
        }
        blocks = {comments, padding};

        for (const auto& [name, value] : written.tags)
        {
            FLAC__StreamMetadata_VorbisComment_Entry entry{};
            if (FLAC__metadata_object_vorbiscomment_entry_from_name_value_pair(&entry, name.c_str(), value.c_str()))
                FLAC__metadata_object_vorbiscomment_append_comment(comments, entry, false);
        }
        padding->length = 8192;   // room to change the tags later without writing the file again

        FLAC__stream_encoder_set_channels(encoder, static_cast<std::uint32_t>(written.channels));
        FLAC__stream_encoder_set_bits_per_sample(encoder, static_cast<std::uint32_t>(written.bits));
        FLAC__stream_encoder_set_sample_rate(encoder, static_cast<std::uint32_t>(written.sampleRate));
        FLAC__stream_encoder_set_compression_level(encoder, static_cast<std::uint32_t>(std::clamp(written.compression, 0, 8)));
        FLAC__stream_encoder_set_metadata(encoder, blocks.data(), static_cast<std::uint32_t>(blocks.size()));

        if (FLAC__stream_encoder_init_stream(encoder, &write, &seek, &tell, nullptr, this) != FLAC__STREAM_ENCODER_INIT_STATUS_OK)
        {
            error = stream.error ? stream.error : make_error_code(AudioFileError::badSettings);
            finished = true;
        }
    }

private:
    void writeFrames(const float* const* channels, std::size_t frames, std::error_code& error) override
    {
        samples.resize(static_cast<std::size_t>(written.channels));
        std::vector<const FLAC__int32*> pointers;
        for (int c = 0; c < written.channels; ++c)
        {
            auto& out = samples[static_cast<std::size_t>(c)];
            out.resize(frames);
            for (std::size_t i = 0; i < frames; ++i)
                out[i] = static_cast<FLAC__int32>(wholeFor(channels[c][i], written.bits));
            pointers.push_back(out.data());
        }

        if (!FLAC__stream_encoder_process(encoder, pointers.data(), static_cast<std::uint32_t>(frames)))
            error = stream.error ? stream.error : std::make_error_code(std::errc::io_error);
    }

    void finishFile(std::error_code& error) override
    {
        if (!FLAC__stream_encoder_finish(encoder) && !stream.error)
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

    static FLAC__StreamEncoderWriteStatus write(const FLAC__StreamEncoder*, const FLAC__byte buffer[], size_t bytes, std::uint32_t,
                                                std::uint32_t, void* self)
    {
        return static_cast<FlacWriter*>(self)->stream.write(buffer, bytes) ? FLAC__STREAM_ENCODER_WRITE_STATUS_OK
                                                                           : FLAC__STREAM_ENCODER_WRITE_STATUS_FATAL_ERROR;
    }

    static FLAC__StreamEncoderSeekStatus seek(const FLAC__StreamEncoder*, FLAC__uint64 at, void* self)
    {
        static_cast<FlacWriter*>(self)->stream.position = at;
        return FLAC__STREAM_ENCODER_SEEK_STATUS_OK;
    }

    static FLAC__StreamEncoderTellStatus tell(const FLAC__StreamEncoder*, FLAC__uint64* at, void* self)
    {
        *at = static_cast<FlacWriter*>(self)->stream.position;
        return FLAC__STREAM_ENCODER_TELL_STATUS_OK;
    }

    std::unique_ptr<RandomAccessFile> file;
    FileStream stream;
    FLAC__StreamEncoder* encoder = nullptr;
    std::vector<FLAC__StreamMetadata*> blocks;
    std::vector<std::vector<FLAC__int32>> samples;
};
}

std::unique_ptr<AudioFileReader> openFlac(std::unique_ptr<RandomAccessFile> file, const std::filesystem::path& path,
                                          std::error_code& error)
{
    auto reader = std::make_unique<FlacReader>(std::move(file), path);
    reader->start(error);
    if (error)
        return nullptr;
    return reader;
}

std::unique_ptr<AudioFileWriter> createFlac(std::unique_ptr<RandomAccessFile> file, const std::filesystem::path& path,
                                            const AudioFileSettings& settings, std::error_code& error)
{
    auto writer = std::make_unique<FlacWriter>(std::move(file), path, settings);
    writer->start(error);
    if (error)
        return nullptr;
    return writer;
}

}
