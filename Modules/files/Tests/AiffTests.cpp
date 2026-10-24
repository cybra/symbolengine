#include "Pattern.h"
#include "Scratch.h"

#include <symbolengine/core/Files.h>
#include <symbolengine/files/AudioFile.h>

#include <catch2/catch_test_macros.hpp>

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

std::uint32_t big32(const std::string& bytes, std::size_t at)
{
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i)
        value = value << 8 | static_cast<unsigned char>(bytes[at + i]);
    return value;
}
}

TEST_CASE("AIFF and AIFF-C files written by other programs read sample for sample")
{
    const Expected files[] = {
        {"pcm16-stereo-44100.aiff", 44100, 2, 16, false},        {"pcm24-mono-48000.aiff", 48000, 1, 24, false},
        {"pcm8-mono-22050.aiff", 22050, 1, 8, false},            {"float32-stereo-48000.aifc", 48000, 2, 32, true},
        {"sowt16-stereo-44100.aifc", 44100, 2, 16, false},       {"float64-mono-48000.aifc", 48000, 1, 64, true},
        {"pcm24-stereo-48000-apple.aiff", 48000, 2, 24, false},
    };

    for (const auto& expected : files)
    {
        CAPTURE(expected.name);
        auto reader = files::openAudio(reference(expected.name));
        const auto& info = reader->info();
        CHECK(info.format == "AIFF");
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

TEST_CASE("AIFF's sample rates, held as 80-bit floats, read back as written")
{
    ScratchFolder folder;
    const auto file = folder.path / "rate.aiff";
    const float silence[1]{};
    const float* channels[1]{silence};

    for (const double rate : {8000.0, 11025.0, 22050.0, 44100.0, 48000.0, 88200.0, 96000.0, 192000.0, 384000.0, 44056.0, 47952.047952})
    {
        CAPTURE(rate);
        {
            auto writer = files::createAudio(file, {"AIFF", rate, 1, 16, false, 0, {}});
            writer->write(channels, 1);
        }
        CHECK(files::openAudio(file)->info().sampleRate == rate);
    }
}

TEST_CASE("AIFFs of whole numbers are written as AIFF, floats as AIFF-C, with their sizes and length")
{
    ScratchFolder folder;
    const auto file = folder.path / "written.aiff";
    const float samples[5]{0.5f, -0.5f, 0.25f, 1.0f, -1.0f};
    const float* channels[1]{samples};

    {
        auto writer = files::createAudio(file, {"AIFF", 44100, 1, 24, false, 0, {}});
        writer->write(channels, 5);
        writer->finish();
    }
    auto bytes = files::read(file);
    CHECK(bytes.substr(0, 4) == "FORM");
    CHECK(big32(bytes, 4) == bytes.size() - 8);
    CHECK(bytes.substr(8, 4) == "AIFF");
    CHECK(bytes.substr(12, 4) == "COMM");
    CHECK(big32(bytes, 16) == 18);
    CHECK(big32(bytes, 22) == 5);
    CHECK(bytes.substr(38, 4) == "SSND");
    CHECK(big32(bytes, 42) == 8 + 15);
    CHECK(bytes.size() % 2 == 0);

    {
        auto writer = files::createAudio(file, {"AIFF", 44100, 1, 32, true, 0, {}});
        writer->write(channels, 5);
    }
    bytes = files::read(file);
    CHECK(bytes.substr(8, 4) == "AIFC");
    CHECK(bytes.substr(12, 4) == "FVER");
    const auto reader = files::openAudio(file);
    CHECK(reader->info().floatingPoint);
    CHECK(pattern::readAll(*reader)[0] == std::vector<float>(samples, samples + 5));
}

TEST_CASE("An AIFF cut short reads to its last whole frame")
{
    ScratchFolder folder;
    const auto file = folder.path / "cut.aiff";
    std::vector<float> samples(1000);
    for (std::size_t i = 0; i < samples.size(); ++i)
        samples[i] = pattern::read(i, 0, 16);
    const float* channels[1]{samples.data()};

    {
        auto writer = files::createAudio(file, {"AIFF", 48000, 1, 16, false, 0, {}});
        writer->write(channels, 1000);
    }
    auto bytes = files::read(file);
    bytes.resize(bytes.size() - 1001);
    files::writeSafely(file, bytes);

    auto reader = files::openAudio(file);
    CHECK(reader->info().frames == 499);
    CHECK(pattern::readAll(*reader)[0][498] == samples[498]);
}

TEST_CASE("An AIFF flushed has the sizes and length of the samples so far in its header")
{
    ScratchFolder folder;
    const auto file = folder.path / "flushed.aiff";
    const float samples[7]{0.5f, -0.5f, 0.25f, 0.75f, -1.0f, 0.125f, -0.125f};
    const float* first[1]{samples};
    const float* rest[1]{samples + 5};

    // 24-bit mono, so five samples and seven are each followed by a pad byte.
    auto writer = files::createAudio(file, {"AIFF", 44100, 1, 24, false, 0, {}});
    writer->write(first, 5);
    writer->flush();
    auto bytes = files::read(file);
    CHECK(big32(bytes, 4) == bytes.size() - 8);
    CHECK(big32(bytes, 22) == 5);
    CHECK(big32(bytes, 42) == 8 + 15);
    CHECK(bytes.size() % 2 == 0);

    writer->write(rest, 2);
    writer->flush();
    bytes = files::read(file);
    CHECK(big32(bytes, 4) == bytes.size() - 8);
    CHECK(big32(bytes, 22) == 7);
    CHECK(big32(bytes, 42) == 8 + 21);
    CHECK(pattern::readAll(*files::openAudio(file))[0] == std::vector<float>(samples, samples + 7));
}
