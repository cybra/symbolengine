// A small program that converts an audio file to the format the new file's extension asks for: WAV, AIFF, FLAC, Ogg
// Vorbis or Opus. It reads and writes a stretch at a time, so a file of any length takes little memory, and says the
// rate, channels and length of both files. Given --tone and a folder instead, it writes a tone there, converts it to
// every format and checks what comes back, which is how it runs as a test with no file of its own.

#include <symbolengine/core.h>
#include <symbolengine/files.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

using namespace symbolengine;

namespace
{

constexpr std::size_t stretch = 4096;

constexpr std::pair<const char*, const char*> formats[] = {
    {".wav", "WAV"}, {".aif", "AIFF"}, {".aiff", "AIFF"}, {".flac", "FLAC"}, {".ogg", "Ogg Vorbis"}, {".opus", "Opus"}};

const char* formatFor(const std::filesystem::path& file)
{
    const auto asked = text::toLowerAscii(paths::toUtf8(file.extension()));

    for (const auto& [extension, name] : formats)
        if (asked == extension)
            return name;

    return nullptr;
}

void say(const std::filesystem::path& file, const AudioInfo& info)
{
    std::printf("%s: %s, %g Hz, channels: %d, frames: %llu\n", paths::toUtf8(file.filename()).c_str(),
                info.format.c_str(), info.sampleRate, info.channels, static_cast<unsigned long long>(info.frames));
}

// A new file with the rate, channels and tags of the one it comes from. Bits are kept where the format holds them, and
// a file that keeps none, such as an MP3, becomes 24 bits.
AudioFileSettings settingsFrom(const AudioInfo& info, const std::string& format)
{
    AudioFileSettings settings;
    settings.format = format;
    settings.sampleRate = info.sampleRate;
    settings.channels = info.channels;
    settings.tags = info.tags;
    settings.floatingPoint = info.floatingPoint && format != "FLAC";

    if (format == "FLAC")
        settings.bits = !info.floatingPoint && info.bits > 0 && info.bits <= 16 ? 16 : 24;
    else if (info.bits > 0 && info.bits % 8 == 0)
        settings.bits = info.bits;

    return settings;
}

void convert(const std::filesystem::path& from, const std::filesystem::path& to, const std::string& format)
{
    const auto reader = files::openAudio(from);
    const auto& info = reader->info();
    say(from, info);

    const auto writer = files::createAudio(to, settingsFrom(info, format));
    std::vector<std::vector<float>> samples(static_cast<std::size_t>(info.channels), std::vector<float>(stretch));
    std::vector<float*> channels;

    for (auto& channel : samples)
        channels.push_back(channel.data());

    // read() gives 0 only at the end, so a length that was the decoder's estimate does no harm here.
    std::uint64_t at = 0;

    while (const auto got = reader->read(at, channels.data(), stretch))
    {
        writer->write(channels.data(), got);
        at += got;
    }

    // Anything that goes wrong in closing the file is said here, where a writer's destructor would ignore it.
    writer->finish();
    say(to, files::openAudio(to)->info());
}

// Writes a tone, louder on the left, converts it to every format, and checks that each file still holds it.
bool checkEveryFormat(const std::filesystem::path& folder)
{
    constexpr std::size_t frames = 12 * stretch;
    std::vector<float> left(frames), right(frames);
    float* channels[] = {left.data(), right.data()};

    for (std::size_t i = 0; i < frames; ++i)
    {
        left[i] = static_cast<float>(0.5 * std::sin(0.05 * static_cast<double>(i)));
        right[i] = 0.5f * left[i];
    }

    std::filesystem::create_directories(folder);
    const auto source = folder / "source.wav";
    files::createAudio(source, AudioFileSettings{})->write(channels, frames);

    for (const auto& [extension, name] : formats)
    {
        const auto file = folder / (std::string("tone") + extension);
        convert(source, file, name);

        std::ranges::fill(left, 0.0f);
        std::ranges::fill(right, 0.0f);
        const auto reader = files::openAudio(file);
        const auto& info = reader->info();

        if (info.format != name || info.sampleRate != 48000.0 || info.channels != 2 || info.frames != frames ||
            reader->read(0, channels, frames) != frames || std::abs(*std::ranges::max_element(left) - 0.5f) > 0.02f ||
            std::abs(*std::ranges::max_element(right) - 0.25f) > 0.02f)
        {
            std::printf("%s doesn't hold the tone.\n", name);
            return false;
        }
    }

    return true;
}

}

int main(int argc, char** argv)
{
    // A path's own constructor reads main's arguments here, which on Windows are in the system's code page.
    try
    {
        if (argc == 3 && std::string(argv[1]) == "--tone")
            return checkEveryFormat(argv[2]) ? 0 : 1;

        const auto* format = argc == 3 ? formatFor(argv[2]) : nullptr;
        std::error_code notThere;

        // Creating a file replaces any of that name, which would lose the samples still to be read.
        if (format == nullptr || std::filesystem::equivalent(argv[1], argv[2], notThere))
        {
            std::printf("Give it an audio file and a new name ending in .wav, .aiff, .flac, .ogg or .opus.\n");
            return 2;
        }

        convert(argv[1], argv[2], format);
        return 0;
    }
    catch (const std::filesystem::filesystem_error& error)
    {
        std::printf("%s: %s\n", paths::toUtf8(error.path1()).c_str(), error.code().message().c_str());
        return 1;
    }
}
