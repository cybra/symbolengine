#include "symbolengine/files/AudioFile.h"

#include "Formats.h"

#include <algorithm>
#include <cstring>

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

    unsigned char start[12]{};
    if (opened->readAt(0, start, sizeof start, error) < sizeof start)
    {
        if (!error)
            error = AudioFileError::notAudio;
        return nullptr;
    }

    const auto is = [&start](int at, const char* id) { return std::memcmp(start + at, id, 4) == 0; };

    if ((is(0, "RIFF") || is(0, "RF64") || is(0, "BW64")) && is(8, "WAVE"))
        return detail::openWav(std::move(opened), file, error);

    if (is(0, "FORM") && (is(8, "AIFF") || is(8, "AIFC")))
        return detail::openAiff(std::move(opened), file, error);

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

    const auto wav = settings.format == "WAV";
    const auto aiff = settings.format == "AIFF";
    const auto wholeBits = settings.bits == 8 || settings.bits == 16 || settings.bits == 24 || settings.bits == 32;
    const auto floatBits = settings.bits == 32 || settings.bits == 64;

    if ((!wav && !aiff) || settings.channels < 1 || settings.channels > 65535 || !(settings.sampleRate > 0.0) ||
        !(settings.floatingPoint ? floatBits : wholeBits) || (wav && settings.sampleRate > 4294967295.0))
    {
        error = AudioFileError::badSettings;
        return nullptr;
    }

    auto created = detail::RandomAccessFile::create(file, error);
    if (created == nullptr)
        return nullptr;

    return wav ? detail::createWav(std::move(created), file, settings, error)
               : detail::createAiff(std::move(created), file, settings, error);
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
