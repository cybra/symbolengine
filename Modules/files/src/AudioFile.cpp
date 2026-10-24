#include "symbolengine/files/AudioFile.h"

#include "Formats.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string_view>

namespace symbolengine
{

std::size_t AudioFileReader::read(std::uint64_t from, float* const* channels, std::size_t frames, std::error_code& error)
{
    error.clear();
    if (from >= information.frames || frames == 0)
        return 0;
    return readFrames(from, channels, static_cast<std::size_t>(std::min<std::uint64_t>(frames, information.frames - from)),
                      error);
}

std::size_t AudioFileReader::read(std::uint64_t from, float* const* channels, std::size_t frames)
{
    std::error_code error;
    const auto done = read(from, channels, frames, error);
    if (error)
        throw std::filesystem::filesystem_error("can't read the audio file", path, error);
    return done;
}

void AudioFileWriter::write(const float* const* channels, std::size_t frames, std::error_code& error)
{
    error.clear();
    if (finished)
    {
        error = std::make_error_code(std::errc::bad_file_descriptor);
        return;
    }
    if (frames == 0)
        return;

    writeFrames(channels, frames, error);
    if (!error)
        count += frames;
}

void AudioFileWriter::write(const float* const* channels, std::size_t frames)
{
    std::error_code error;
    write(channels, frames, error);
    if (error)
        throw std::filesystem::filesystem_error("can't write the audio file", path, error);
}

void AudioFileWriter::finish(std::error_code& error)
{
    error.clear();
    if (finished)
        return;
    finished = true;
    finishFile(error);
}

void AudioFileWriter::finish()
{
    std::error_code error;
    finish(error);
    if (error)
        throw std::filesystem::filesystem_error("can't finish the audio file", path, error);
}

namespace
{
class Category final : public std::error_category
{
public:
    const char* name() const noexcept override { return "symbolengine.audio-file"; }

    std::string message(int code) const override
    {
        switch (static_cast<AudioFileError>(code))
        {
            case AudioFileError::notAudio:
                return "The file isn't audio of a kind that can be read.";
            case AudioFileError::unsupported:
                return "The file's audio is stored in a way that can't be read.";
            case AudioFileError::damaged:
                return "The file is damaged.";
            case AudioFileError::badSettings:
                return "An audio file can't be written with those settings.";
            case AudioFileError::tooLong:
                return "The audio is too long for the file's format.";
        }
        return "Unknown audio file error.";
    }
};
}

std::error_code make_error_code(AudioFileError error) noexcept
{
    return {static_cast<int>(error), files::audioFileCategory()};
}

namespace files
{

const std::error_category& audioFileCategory() noexcept
{
    static const Category category;
    return category;
}

std::unique_ptr<AudioFileReader> openAudio(const std::filesystem::path& file, std::error_code& error)
{
    error.clear();
    auto opened = detail::RandomAccessFile::open(file, error);
    if (opened == nullptr)
        return nullptr;

    unsigned char start[300]{};
    const auto got = opened->readAt(0, start, sizeof start, error);
    if (error)
        return nullptr;

    const auto is = [&](std::size_t at, std::string_view id)
    { return at + id.size() <= got && std::memcmp(start + at, id.data(), id.size()) == 0; };

    if ((is(0, "RIFF") || is(0, "RF64") || is(0, "BW64")) && is(8, "WAVE"))
        return detail::openWav(std::move(opened), file, error);

    if (is(0, "FORM") && (is(8, "AIFF") || is(8, "AIFC")))
        return detail::openAiff(std::move(opened), file, error);

    // FLAC, after an ID3 tag some programs put first.
    auto flacAt = std::size_t{0};
    if (is(0, "ID3") && got >= 10)
    {
        flacAt = 10 + (static_cast<std::size_t>(start[6] & 0x7F) << 21 | static_cast<std::size_t>(start[7] & 0x7F) << 14 |
                       static_cast<std::size_t>(start[8] & 0x7F) << 7 | static_cast<std::size_t>(start[9] & 0x7F));
        unsigned char marker[4]{};
        if (opened->readAt(flacAt, marker, 4, error) == 4 && std::memcmp(marker, "fLaC", 4) == 0)
            return detail::openFlac(std::move(opened), file, error);
    }
    if (is(0, "fLaC"))
        return detail::openFlac(std::move(opened), file, error);

    // Ogg, by what its first packet starts with, which begins after the page's header and its table of lengths.
    if (is(0, "OggS") && got > 27)
    {
        const auto packet = 27 + static_cast<std::size_t>(start[26]);
        if (is(packet, "\x01vorbis"))
            return detail::openVorbis(std::move(opened), file, error);
        if (is(packet, "OpusHead"))
            return detail::openOpus(std::move(opened), file, error);
        if (is(packet, "\x7f" "FLAC"))
        {
            error = AudioFileError::unsupported;
            return nullptr;
        }
    }

    error = AudioFileError::notAudio;
    return nullptr;
}

std::unique_ptr<AudioFileReader> openAudio(const std::filesystem::path& file)
{
    std::error_code error;
    auto reader = openAudio(file, error);
    if (error)
        throw std::filesystem::filesystem_error("can't open the audio file", file, error);
    return reader;
}

std::unique_ptr<AudioFileWriter> createAudio(const std::filesystem::path& file, const AudioFileSettings& settings,
                                             std::error_code& error)
{
    error.clear();

    const auto& format = settings.format;
    const auto wholeBits = settings.bits == 8 || settings.bits == 16 || settings.bits == 24 || settings.bits == 32;
    const auto floatBits = settings.bits == 32 || settings.bits == 64;
    const auto rate = settings.sampleRate;

    // What each format holds: WAV a rate in 32 bits, FLAC 16 or 24 bits in up to eight channels at a whole rate it
    // can say in its frames' headers, and Ogg Vorbis and Opus up to 255 channels.
    const auto fits = format == "WAV"          ? (settings.floatingPoint ? floatBits : wholeBits) && rate <= 4294967295.0
                    : format == "AIFF"         ? (settings.floatingPoint ? floatBits : wholeBits)
                    : format == "FLAC"         ? !settings.floatingPoint && (settings.bits == 16 || settings.bits == 24) &&
                                                     settings.channels <= 8 && rate == std::floor(rate) && rate <= 655350.0
                    : format == "Ogg Vorbis"   ? settings.channels <= 255 && rate <= 200000.0
                    : format == "Opus"         ? settings.channels <= 255 && rate <= 384000.0
                                               : false;

    if (!fits || settings.channels < 1 || settings.channels > 65535 || !(rate > 0.0))
    {
        error = AudioFileError::badSettings;
        return nullptr;
    }

    auto created = detail::RandomAccessFile::create(file, error);
    if (created == nullptr)
        return nullptr;

    std::unique_ptr<AudioFileWriter> writer;
    if (format == "WAV")
        writer = detail::createWav(std::move(created), file, settings, error);
    else if (format == "AIFF")
        writer = detail::createAiff(std::move(created), file, settings, error);
    else if (format == "FLAC")
        writer = detail::createFlac(std::move(created), file, settings, error);
    else if (format == "Ogg Vorbis")
        writer = detail::createVorbis(std::move(created), file, settings, error);
    else
        writer = detail::createOpus(std::move(created), file, settings, error);

    // A file that couldn't be started isn't left behind.
    if (writer == nullptr)
    {
        std::error_code ignored;
        std::filesystem::remove(file, ignored);
    }
    return writer;
}

std::unique_ptr<AudioFileWriter> createAudio(const std::filesystem::path& file, const AudioFileSettings& settings)
{
    std::error_code error;
    auto writer = createAudio(file, settings, error);
    if (error)
        throw std::filesystem::filesystem_error("can't create the audio file", file, error);
    return writer;
}

}

}
