#pragma once

#include "RandomAccessFile.h"

#include "symbolengine/files/AudioFile.h"

#include <filesystem>
#include <memory>
#include <system_error>

// Each format's reader and writer, which openAudio() and createAudio() choose between.
namespace symbolengine::detail
{

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

/** The formats the system's own decoders read: MPEG-4 audio such as M4A, Core Audio's CAF, AAC in ADTS, and WAV and
    AIFF-C whose samples are coded in ways of their own, such as ADPCM and µ-law. */
enum class SystemFormat
{
    mpeg4,
    caf,
    adts,
    wav,
    aiff
};

/** A file the system's own decoder reads: Apple's AudioToolbox on a Mac, which reads all of them, and Media Foundation
    on Windows, which reads MPEG-4 audio and ADTS; elsewhere none, which is AudioFileError::unsupported. */
std::unique_ptr<AudioFileReader> openSystemAudio(std::unique_ptr<RandomAccessFile> file, const std::filesystem::path& path,
                                                 SystemFormat format, std::error_code& error);

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
