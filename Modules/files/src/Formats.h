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
