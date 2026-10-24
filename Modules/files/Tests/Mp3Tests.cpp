#include "Pattern.h"
#include "Scratch.h"

#include <symbolengine/files/AudioFile.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <string>

using namespace symbolengine;
using pattern::reference;

namespace
{
// The samples LAME's encoder puts before the audio, and those every MP3 decoder adds to them.
constexpr std::size_t lameDelay = 576 + 529;

std::vector<float> from(const std::vector<float>& channel, std::size_t first, std::size_t frames)
{
    return {channel.begin() + static_cast<std::ptrdiff_t>(first), channel.begin() + static_cast<std::ptrdiff_t>(first + frames)};
}

// The loudest of a channel's first samples, which the encoder's delay keeps near silence before a tone at half scale.
float loudestOfFirst(const std::vector<float>& channel, std::size_t frames)
{
    float loudest = 0.0f;
    for (std::size_t i = 0; i < frames; ++i)
        loudest = std::max(loudest, std::abs(channel[i]));
    return loudest;
}
}

TEST_CASE("An MP3 with LAME's information reads its audio alone, each channel's tone in time and in place, from anywhere in it")
{
    // It starts with an ID3 tag, as MP3s often do.
    auto reader = files::openAudio(reference("tones-stereo-44100.mp3"));
    const auto& info = reader->info();
    CHECK(info.format == "MP3");
    CHECK(info.sampleRate == 44100);
    CHECK(info.channels == 2);
    CHECK(info.bits == 0);
    REQUIRE(info.frames == 22050);

    const auto samples = pattern::readAll(*reader);
    for (int c = 0; c < 2; ++c)
    {
        CAPTURE(c);
        CHECK(pattern::toneMatch(samples[static_cast<std::size_t>(c)], c, 44100) > 20.0);
    }

    // Stretches out of order, each the same as where it sits in the whole, as decoding from the start gave it.
    for (const std::uint64_t at : {15000u, 3u, 9000u, 1152u, 0u, 22000u})
    {
        CAPTURE(at);
        const auto part = pattern::stretch(*reader, at, 1000);
        CHECK(part.front().size() == std::min<std::uint64_t>(1000, 22050 - at));
        CHECK(pattern::largestDifference(part, samples, at) < 1.0e-5f);
    }
}

TEST_CASE("An MP3 without that information reads as it decodes, the encoder's and decoder's delay kept")
{
    auto reader = files::openAudio(reference("tone-mono-48000-without-information.mp3"));
    const auto& info = reader->info();
    CHECK(info.format == "MP3");
    CHECK(info.sampleRate == 48000);
    CHECK(info.channels == 1);
    CHECK(info.frames % 1152 == 0);
    REQUIRE(info.frames >= 24000 + lameDelay);

    const auto samples = pattern::readAll(*reader).front();
    CHECK(pattern::toneMatch(from(samples, lameDelay, 24000), 0, 48000) > 20.0);
    CHECK(loudestOfFirst(samples, 500) < 0.05f);
}

TEST_CASE("An MP3 whose information another encoder wrote reads as it decodes, as that encoder may keep other things where LAME keeps the delay")
{
    ScratchFolder folder;
    std::ifstream in(reference("tones-stereo-44100.mp3"), std::ios::binary);
    std::string bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    const auto info = bytes.find("Info");
    const auto lame = bytes.find("LAME", info);
    REQUIRE(info != std::string::npos);
    REQUIRE(lame != std::string::npos);
    REQUIRE(lame - info < 200);
    bytes.replace(lame, 4, "GOGO");
    const auto other = folder.path / "other.mp3";
    std::ofstream(other, std::ios::binary).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));

    auto reader = files::openAudio(other);
    const auto& found = reader->info();
    CHECK(found.frames % 1152 == 0);
    REQUIRE(found.frames >= 22050 + lameDelay);
    const auto samples = pattern::readAll(*reader);
    for (int c = 0; c < 2; ++c)
    {
        CAPTURE(c);
        CHECK(pattern::toneMatch(from(samples[static_cast<std::size_t>(c)], lameDelay, 22050), c, 44100) > 20.0);
        CHECK(loudestOfFirst(samples[static_cast<std::size_t>(c)], 500) < 0.05f);
    }

    // Stretches out of order, each the same as where it sits in the whole.
    for (const std::uint64_t at : {15000u, 3u, 9000u, 0u})
    {
        CAPTURE(at);
        CHECK(pattern::largestDifference(pattern::stretch(*reader, at, 1000), samples, at) < 1.0e-5f);
    }
}

TEST_CASE("An MP3 cut short reads what it holds, and a file that only starts like one isn't audio")
{
    ScratchFolder folder;
    std::ifstream in(reference("tones-stereo-44100.mp3"), std::ios::binary);
    const std::vector<char> whole{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};

    const auto cut = folder.path / "cut.mp3";
    std::ofstream(cut, std::ios::binary).write(whole.data(), static_cast<std::streamsize>(whole.size() / 2));
    auto reader = files::openAudio(cut);
    REQUIRE(reader->info().frames == 22050);
    std::vector<float> left(22050), right(22050);
    float* channels[] = {left.data(), right.data()};
    const auto got = reader->read(0, channels, 22050);
    CHECK(got > 5000);
    CHECK(got < 22050);

    const auto noise = folder.path / "noise.mp3";
    {
        std::ofstream out(noise, std::ios::binary);
        out.put(static_cast<char>(0xFF)).put(static_cast<char>(0xFB));
        for (int i = 0; i < 20000; ++i)
            out.put(static_cast<char>((i * 7919) % 251));
    }
    std::error_code error;
    CHECK(files::openAudio(noise, error) == nullptr);
    CHECK(error == AudioFileError::notAudio);
}
