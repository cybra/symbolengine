#include "Formats.h"

// Linux has no decoder of its own for MPEG-4 audio, CAF, AAC or the rest, which come after the first release.
namespace symbolengine::detail
{

std::vector<std::string> systemExtensions()
{
    return {};
}

std::unique_ptr<AudioFileReader> openSystemAudio(std::unique_ptr<RandomAccessFile>, const std::filesystem::path&, SystemFormat,
                                                 std::error_code& error)
{
    error = AudioFileError::unsupported;
    return nullptr;
}

}
