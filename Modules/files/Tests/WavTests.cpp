#include "Pattern.h"
#include "Scratch.h"

#include <symbolengine/core/Files.h>
#include <symbolengine/files/AudioFile.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <fstream>
#include <string>

using namespace symbolengine;
using pattern::reference;

namespace
{
struct Expected
{
    const char* name;
    double rate;
    int channels;
    int bits;
    bool floatingPoint;
};

std::uint32_t little32(const std::string& bytes, std::size_t at)
{
    std::uint32_t value = 0;
    for (int i = 3; i >= 0; --i)
        value = value << 8 | static_cast<unsigned char>(bytes[at + static_cast<std::size_t>(i)]);
    return value;
}

std::uint16_t little16(const std::string& bytes, std::size_t at)
{
    return static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[at]) | static_cast<unsigned char>(bytes[at + 1]) << 8);
}

std::string littleBytes(std::uint32_t value, int count)
{
    std::string bytes;
    for (int i = 0; i < count; ++i)
        bytes += static_cast<char>(value >> (8 * i) & 0xFF);
    return bytes;
}

// A WAV of 16-bit stereo made byte by byte, with the chunks given before its data.
std::string handmadeWav(const std::string& chunksBefore, const std::string& samples, std::uint32_t dataSize)
{
    std::string fmt = "fmt " + littleBytes(16, 4) + littleBytes(1, 2) + littleBytes(2, 2) + littleBytes(48000, 4) +
                      littleBytes(48000 * 4, 4) + littleBytes(4, 2) + littleBytes(16, 2);
    std::string body = "WAVE" + fmt + chunksBefore + "data" + littleBytes(dataSize, 4) + samples;
    return "RIFF" + littleBytes(static_cast<std::uint32_t>(body.size()), 4) + body;
}

// Stereo 16-bit samples of the pattern, as a WAV holds them.
std::string patternSamples(std::uint64_t frames)
{
    std::string bytes;
    for (std::uint64_t frame = 0; frame < frames; ++frame)
        for (int channel = 0; channel < 2; ++channel)
            bytes += littleBytes(static_cast<std::uint32_t>(pattern::whole(frame, channel, 16)), 2);
    return bytes;
}
}

TEST_CASE("WAV files written by other programs read sample for sample")
{
    const Expected files[] = {
        {"pcm16-stereo-44100.wav", 44100, 2, 16, false},     {"pcm24-mono-48000.wav", 48000, 1, 24, false},
        {"pcm32-stereo-96000.wav", 96000, 2, 32, false},     {"unsigned8-mono-22050.wav", 22050, 1, 8, false},
        {"float32-stereo-48000.wav", 48000, 2, 32, true},    {"float64-mono-48000.wav", 48000, 1, 64, true},
        {"pcm24-5.1-48000.wav", 48000, 6, 24, false},        {"rf64-pcm16-stereo-48000.wav", 48000, 2, 16, false},
        {"bwf-pcm24-stereo-48000.wav", 48000, 2, 24, false},
    };

    for (const auto& expected : files)
    {
        CAPTURE(expected.name);
        auto reader = files::openAudio(reference(expected.name));
        const auto& info = reader->info();
        CHECK(info.format == "WAV");
        CHECK(info.sampleRate == expected.rate);
        CHECK(info.channels == expected.channels);
        CHECK(info.bits == expected.bits);
        CHECK(info.floatingPoint == expected.floatingPoint);
        REQUIRE(info.frames == pattern::referenceFrames);

        const auto samples = pattern::readAll(*reader);
        int wrong = 0;
        for (int channel = 0; channel < info.channels; ++channel)
            for (std::uint64_t frame = 0; frame < info.frames; ++frame)
            {
                const auto want = expected.floatingPoint ? static_cast<float>(pattern::floating(frame, channel))
                                                         : pattern::read(frame, channel, expected.bits);
                wrong += samples[static_cast<std::size_t>(channel)][frame] != want ? 1 : 0;
            }
        CHECK(wrong == 0);
    }
}

TEST_CASE("A WAV's speakers are read from WAVE_FORMAT_EXTENSIBLE")
{
    CHECK(files::openAudio(reference("pcm24-5.1-48000.wav"))->info().channelMask == 0x3F);
    CHECK(files::openAudio(reference("pcm16-stereo-44100.wav"))->info().channelMask == 0);
}

TEST_CASE("An RF64 file's sizes come from its ds64 chunk, which isn't kept with the other chunks")
{
    const auto reader = files::openAudio(reference("rf64-pcm16-stereo-48000.wav"));
    CHECK(reader->info().frames == pattern::referenceFrames);
    for (const auto& chunk : reader->info().chunks)
        CHECK((chunk.id != "ds64" && chunk.id != "JUNK" && chunk.id != "fmt " && chunk.id != "data"));
}

TEST_CASE("A WAV whose samples are compressed is read by the Mac's decoder, and is said elsewhere to be stored in a way that can't be read")
{
    std::error_code error;
    auto reader = files::openAudio(reference("adpcm-mono-44100.wav"), error);
#if defined(__APPLE__)
    REQUIRE(reader != nullptr);
    CHECK(!error);
    CHECK(reader->info().format == "WAV");
    CHECK(reader->info().channels == 1);
    CHECK(reader->info().sampleRate == 44100);

    // Microsoft's ADPCM keeps its samples in blocks of 1018, the last filled out.
    REQUIRE(reader->info().frames == 2036);
    const auto samples = pattern::readAll(*reader).front();
    CHECK(std::any_of(samples.begin(), samples.end(), [](float sample) { return std::abs(sample) > 0.1f; }));
#else
    CHECK(reader == nullptr);
    CHECK(error == AudioFileError::unsupported);
#endif
}

TEST_CASE("A WAV cut short reads to its last whole frame, and one saying no size reads to its end")
{
    ScratchFolder folder;
    const auto file = folder.path / "cut.wav";

    // 1000 frames written, then the file cut half way through a frame.
    files::writeSafely(file, handmadeWav("", patternSamples(1000), 4000));
    auto bytes = files::read(file);
    bytes.resize(bytes.size() - 2000 - 2);
    files::writeSafely(file, bytes);
    {
        // Let go of before the file is written again, which Windows doesn't allow while it's open.
        auto reader = files::openAudio(file);
        CHECK(reader->info().frames == 499);
        CHECK(pattern::readAll(*reader)[1][498] == pattern::read(498, 1, 16));
    }

    for (const std::uint32_t said : {0u, 0xFFFFFFFFu})
    {
        files::writeSafely(file, handmadeWav("", patternSamples(1000), said));
        CHECK(files::openAudio(file)->info().frames == 1000);
    }
}

TEST_CASE("A WAV's chunks of odd length are read whether their pad byte is there or not")
{
    ScratchFolder folder;
    const auto file = folder.path / "odd.wav";

    for (const bool padded : {true, false})
    {
        CAPTURE(padded);
        const auto chunks = "LIST" + littleBytes(3, 4) + "abc" + (padded ? std::string(1, '\0') : std::string()) +
                            "iXML" + littleBytes(4, 4) + "<a/>";
        files::writeSafely(file, handmadeWav(chunks, patternSamples(10), 40));
        const auto reader = files::openAudio(file);
        REQUIRE(reader->info().chunks.size() == 2);
        CHECK(reader->info().chunks[0] == AudioChunk{"LIST", "abc"});
        CHECK(reader->info().chunks[1] == AudioChunk{"iXML", "<a/>"});
        CHECK(reader->info().frames == 10);
    }
}

TEST_CASE("WAVs are written as plain PCM up to stereo, floats with a fact chunk, and more channels as extensible")
{
    ScratchFolder folder;
    const auto file = folder.path / "written.wav";
    const float silence[4]{};
    const float* channels[6]{silence, silence, silence, silence, silence, silence};

    const auto writeAndRead = [&](AudioFileSettings settings)
    {
        auto writer = files::createAudio(file, settings);
        writer->write(channels, 4);
        writer->finish();
        return files::read(file);
    };

    // RIFF, then a JUNK chunk of 28 bytes where a ds64 chunk fits, then fmt.
    auto bytes = writeAndRead({"WAV", 44100, 2, 16, false, 0, {}});
    CHECK(bytes.substr(0, 4) == "RIFF");
    CHECK(little32(bytes, 4) == bytes.size() - 8);
    CHECK(bytes.substr(12, 4) == "JUNK");
    CHECK(little32(bytes, 16) == 28);
    CHECK(bytes.substr(48, 4) == "fmt ");
    CHECK(little32(bytes, 52) == 16);
    CHECK(little16(bytes, 56) == 1);
    CHECK(little16(bytes, 58) == 2);
    CHECK(little32(bytes, 60) == 44100);
    CHECK(little32(bytes, 64) == 44100 * 4);
    CHECK(little16(bytes, 68) == 4);
    CHECK(little16(bytes, 70) == 16);
    CHECK(bytes.substr(72, 4) == "data");
    CHECK(little32(bytes, 76) == 16);

    bytes = writeAndRead({"WAV", 48000, 2, 32, true, 0, {}});
    CHECK(little32(bytes, 52) == 18);
    CHECK(little16(bytes, 56) == 3);
    CHECK(bytes.substr(74, 4) == "fact");
    CHECK(little32(bytes, 82) == 4);

    bytes = writeAndRead({"WAV", 48000, 6, 24, false, 0, {}});
    CHECK(little32(bytes, 52) == 40);
    CHECK(little16(bytes, 56) == 0xFFFE);
    CHECK(little16(bytes, 74) == 24);
    CHECK(little32(bytes, 76) == 0x3F);
    CHECK(little16(bytes, 80) == 1);

    bytes = writeAndRead({"WAV", 48000, 2, 24, false, 0x3, {}});
    CHECK(little16(bytes, 56) == 0xFFFE);
    CHECK(files::openAudio(file)->info().channelMask == 0x3);
}

TEST_CASE("A WAV's samples of odd length are followed by a pad byte the sizes leave out of the data")
{
    ScratchFolder folder;
    const auto file = folder.path / "odd.wav";
    const float samples[3]{0.25f, -0.5f, 0.75f};
    const float* channels[1]{samples};

    auto writer = files::createAudio(file, {"WAV", 48000, 1, 8, false, 0, {}});
    writer->write(channels, 3);
    writer->finish();

    const auto bytes = files::read(file);
    CHECK(bytes.size() % 2 == 0);
    CHECK(little32(bytes, 4) == bytes.size() - 8);
    const auto reader = files::openAudio(file);
    CHECK(reader->info().frames == 3);
    CHECK(pattern::readAll(*reader)[0] == std::vector<float>{0.25f, -0.5f, 0.75f});
}

TEST_CASE("A WAV flushed has the sizes of the samples so far in its header, and the next samples go on after them")
{
    ScratchFolder folder;
    const auto file = folder.path / "flushed.wav";
    const float samples[5]{0.25f, -0.5f, 0.75f, 0.125f, -0.25f};
    const float* first[1]{samples};
    const float* rest[1]{samples + 3};

    // 8-bit mono, so three samples are followed by a pad byte, which the next two write over.
    auto writer = files::createAudio(file, {"WAV", 48000, 1, 8, false, 0, {}});
    writer->write(first, 3);
    writer->flush();
    auto bytes = files::read(file);
    CHECK(bytes.size() == 80 + 4);
    CHECK(little32(bytes, 4) == bytes.size() - 8);
    CHECK(little32(bytes, 76) == 3);

    writer->write(rest, 2);
    writer->flush();
    bytes = files::read(file);
    CHECK(bytes.size() == 80 + 6);
    CHECK(little32(bytes, 4) == bytes.size() - 8);
    CHECK(little32(bytes, 76) == 5);
    CHECK(pattern::readAll(*files::openAudio(file))[0] == std::vector<float>(samples, samples + 5));

    // A float WAV's fact chunk has its length so far too.
    auto floats = files::createAudio(folder.path / "floats.wav", {"WAV", 48000, 1, 32, true, 0, {}});
    floats->write(first, 3);
    floats->flush();
    CHECK(little32(files::read(folder.path / "floats.wav"), 82) == 3);
}

// Writes a file past 4 GB, which becomes RF64; hidden, as it takes a while and 4 GB of disk. Run it with its tag.
TEST_CASE("A WAV past 4 GB becomes RF64 and reads back whole", "[.big]")
{
    ScratchFolder folder;
    const auto file = folder.path / "big.wav";
    constexpr std::size_t block = 1 << 16;
    std::vector<float> samples(block);
    for (std::size_t i = 0; i < block; ++i)
        samples[i] = static_cast<float>(pattern::whole(i, 0, 16)) / 32768.0f;
    const float* channels[2]{samples.data(), samples.data()};

    // 4.1 GB of 16-bit stereo.
    const std::uint64_t frames = 1'100'000'000;
    {
        auto writer = files::createAudio(file, {"WAV", 48000, 2, 16, false, 0, {}});
        for (std::uint64_t done = 0; done < frames; done += block)
            writer->write(channels, static_cast<std::size_t>(std::min<std::uint64_t>(block, frames - done)));
        writer->finish();
    }

    auto reader = files::openAudio(file);
    char start[4]{};
    std::ifstream(file, std::ios::binary).read(start, 4);
    CHECK(std::string(start, 4) == "RF64");
    REQUIRE(reader->info().frames == frames);
    std::vector<float> left(10), right(10);
    float* out[2]{left.data(), right.data()};
    CHECK(reader->read(frames - 10, out, 10) == 10);
    const auto last = (frames - 10) % block;
    CHECK(left[0] == samples[last]);
    CHECK(right[9] == samples[(frames - 1) % block]);
}

// Flushes a file past 4 GB, which becomes RF64 before it is finished; hidden as the one above is.
TEST_CASE("A WAV flushed past 4 GB becomes RF64 in place, and reads whole while it is still being written", "[.big]")
{
    ScratchFolder folder;
    const auto file = folder.path / "big.wav";
    constexpr std::size_t block = 1 << 16;
    std::vector<float> samples(block);
    for (std::size_t i = 0; i < block; ++i)
        samples[i] = static_cast<float>(pattern::whole(i, 0, 16)) / 32768.0f;
    const float* channels[2]{samples.data(), samples.data()};

    const auto little64 = [](const std::string& bytes, std::size_t at)
    { return static_cast<std::uint64_t>(little32(bytes, at)) | static_cast<std::uint64_t>(little32(bytes, at + 4)) << 32; };
    const auto start = [&]
    {
        std::string bytes(48, '\0');
        std::ifstream(file, std::ios::binary).read(bytes.data(), 48);
        return bytes;
    };

    // 4.1 GB of 16-bit stereo.
    const std::uint64_t frames = 1'100'000'000;
    auto writer = files::createAudio(file, {"WAV", 48000, 2, 16, false, 0, {}});
    for (std::uint64_t done = 0; done < frames; done += block)
        writer->write(channels, static_cast<std::size_t>(std::min<std::uint64_t>(block, frames - done)));
    writer->flush();

    auto bytes = start();
    CHECK(bytes.substr(0, 4) == "RF64");
    CHECK(little32(bytes, 4) == 0xFFFFFFFF);
    CHECK(bytes.substr(12, 4) == "ds64");
    CHECK(little64(bytes, 20) == std::filesystem::file_size(file) - 8);
    CHECK(little64(bytes, 28) == frames * 4);
    CHECK(little64(bytes, 36) == frames);
    CHECK(files::openAudio(file)->info().frames == frames);

    writer->write(channels, 10);
    writer->flush();
    bytes = start();
    CHECK(little64(bytes, 28) == (frames + 10) * 4);
    const auto reader = files::openAudio(file);
    REQUIRE(reader->info().frames == frames + 10);
    std::vector<float> left(10), right(10);
    float* out[2]{left.data(), right.data()};
    CHECK(reader->read(frames, out, 10) == 10);
    CHECK(left == std::vector<float>(samples.begin(), samples.begin() + 10));
    writer->finish();
}

namespace
{
void addLittle32(std::string& bytes, std::uint32_t value)
{
    for (int shift = 0; shift < 32; shift += 8)
        bytes += static_cast<char>((value >> shift) & 0xFF);
}

void addLittle64(std::string& bytes, std::uint64_t value)
{
    for (int shift = 0; shift < 64; shift += 8)
        bytes += static_cast<char>((value >> shift) & 0xFF);
}

// The start of an RF64 file whose ds64 chunk gives the chunk "abcd" the size, then that chunk.
std::string rf64Saying(std::uint64_t size)
{
    std::string bytes = "RF64";
    addLittle32(bytes, 0xFFFFFFFF);
    bytes += "WAVEds64";
    addLittle32(bytes, 40);
    addLittle64(bytes, 0);
    addLittle64(bytes, 0);
    addLittle64(bytes, 0);
    addLittle32(bytes, 1);
    bytes += "abcd";
    addLittle64(bytes, size);
    bytes += "abcd";
    addLittle32(bytes, 0xFFFFFFFF);
    return bytes;
}
}

TEST_CASE("A chunk saying more than the file holds ends the walk, whatever its size says")
{
    ScratchFolder folder;
    const auto file = folder.path / "chunks.wav";

    // Sizes that take the walk back onto the chunk itself, back to the file's start, past the end of 64 bits, and past
    // the end of the file.
    const auto header = rf64Saying(0).size();
    for (const std::uint64_t size : {std::uint64_t{0} - 8, std::uint64_t{0} - header, std::uint64_t{0} - 1,
                                     std::uint64_t{1} << 62, std::uint64_t{1} << 40})
    {
        CAPTURE(size);
        files::writeSafely(file, rf64Saying(size));
        std::error_code error;
        CHECK(files::openAudio(file, error) == nullptr);
        CHECK(error == AudioFileError::damaged);
    }

    // A file's samples before such a chunk still read.
    auto bytes = rf64Saying(std::uint64_t{0} - 8);
    bytes.resize(bytes.size() - 8);
    bytes += "fmt ";
    addLittle32(bytes, 16);
    bytes += std::string("\x01\x00\x01\x00", 4);
    addLittle32(bytes, 48000);
    addLittle32(bytes, 96000);
    bytes += std::string("\x02\x00\x10\x00", 4);
    bytes += "data";
    addLittle32(bytes, 8);
    bytes += std::string("\x01\x00\x02\x00\x03\x00\x04\x00", 8);
    bytes += "abcd";
    addLittle32(bytes, 0xFFFFFFFF);
    files::writeSafely(file, bytes);
    const auto reader = files::openAudio(file);
    CHECK(reader->info().frames == 4);
    CHECK(reader->info().sampleRate == 48000);
}
