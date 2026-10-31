#pragma once

#include "RandomAccessFile.h"

#include "symbolengine/files/AudioFile.h"

#include <filesystem>
#include <memory>
#include <system_error>

// Each format's reader and writer, which openAudio() and createAudio() choose between.
namespace symbolengine::detail
{

/** Where a reader's next frame is when a seek failed and left its decoder who knows where: asked for no frame, so the next
    read seeks again. */
inline constexpr std::uint64_t nowhere = ~std::uint64_t{0};

/** The most channels a file is taken to have, past anything real, as a header saying 65535 costs gigabytes to read. */
inline constexpr int mostChannels = 1024;

/** The most bytes of a WAV's or AIFF's other chunks kept in memory, whatever a file holds. */
inline constexpr std::uint64_t mostKeptInAll = 256u * 1024 * 1024;

std::unique_ptr<AudioFileReader> openWav(std::unique_ptr<RandomAccessFile> file, const std::filesystem::path& path,
                                         std::error_code& error);
std::unique_ptr<AudioFileReader> openAiff(std::unique_ptr<RandomAccessFile> file, const std::filesystem::path& path,
                                          std::error_code& error);
std::unique_ptr<AudioFileReader> openFlac(std::unique_ptr<RandomAccessFile> file, const std::filesystem::path& path,
                                          std::error_code& error);
std::unique_ptr<AudioFileReader> openVorbis(std::unique_ptr<RandomAccessFile> file, const std::filesystem::path& path,
                                            std::error_code& error);
std::unique_ptr<AudioFileReader> openOpus(std::unique_ptr<RandomAccessFile> file, const std::filesystem::path& path,
                                          std::error_code& error);
std::unique_ptr<AudioFileReader> openMp3(std::unique_ptr<RandomAccessFile> file, const std::filesystem::path& path,
                                         std::error_code& error);

/** The formats the system's own decoders read: MPEG-4 audio such as M4A, Core Audio's CAF, AAC in ADTS, WAV and
    AIFF-C whose samples are coded in ways of their own, such as ADPCM and µ-law, Windows Media's ASF, and a file of
    any other kind, which the system tells by what it holds. */
enum class SystemFormat
{
    mpeg4,
    caf,
    adts,
    wav,
    aiff,
    asf,
    other
};

/** A file the system's own decoder reads: Apple's AudioToolbox on a Mac, which reads all of them but ASF, and Media
    Foundation on Windows, which reads MPEG-4 audio, ADTS and ASF; elsewhere none, which is AudioFileError::unsupported. */
std::unique_ptr<AudioFileReader> openSystemAudio(std::unique_ptr<RandomAccessFile> file, const std::filesystem::path& path,
                                                 SystemFormat format, std::error_code& error);

/** The extensions of the files the system's own decoders read, with their dots, in lower case. */
std::vector<std::string> systemExtensions();

std::unique_ptr<AudioFileWriter> createWav(std::unique_ptr<RandomAccessFile> file, const std::filesystem::path& path,
                                           const AudioFileSettings& settings, std::error_code& error);
std::unique_ptr<AudioFileWriter> createAiff(std::unique_ptr<RandomAccessFile> file, const std::filesystem::path& path,
                                            const AudioFileSettings& settings, std::error_code& error);
std::unique_ptr<AudioFileWriter> createFlac(std::unique_ptr<RandomAccessFile> file, const std::filesystem::path& path,
                                            const AudioFileSettings& settings, std::error_code& error);
std::unique_ptr<AudioFileWriter> createVorbis(std::unique_ptr<RandomAccessFile> file, const std::filesystem::path& path,
                                              const AudioFileSettings& settings, std::error_code& error);
std::unique_ptr<AudioFileWriter> createOpus(std::unique_ptr<RandomAccessFile> file, const std::filesystem::path& path,
                                            const AudioFileSettings& settings, std::error_code& error);

}
