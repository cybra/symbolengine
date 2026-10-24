#include "Mp4.h"
#include "Pattern.h"

#include <symbolengine/files/AudioFile.h>

#include <catch2/catch_test_macros.hpp>

#include <string_view>

using namespace symbolengine;
using pattern::reference;

namespace
{
// Where a channel's tone starts after the silence before it, found by how near the samples come to silence and then
// the tone from each place it could start; the silence tells apart places a period of the tone apart.
[[maybe_unused]] std::size_t toneStart(const std::vector<float>& samples, int channel, double rate, std::size_t latest, std::size_t length)
{
    std::size_t best = 0;
    auto least = 1e30;
    for (std::size_t start = 0; start <= latest && start + length <= samples.size(); ++start)
    {
        double difference = 0.0;
        for (std::size_t frame = 0; frame < start + length; ++frame)
        {
            const auto want = frame < start ? 0.0 : pattern::tone(frame - start, channel, rate);
            difference += (samples[frame] - want) * (samples[frame] - want);
        }
        if (difference < least)
        {
            least = difference;
            best = start;
        }
    }
    return best;
}
}

// M4A, AAC and CAF, which the system's own decoders read: on a Mac all three, on Windows the first two and on Linux
// none.
#if defined(__APPLE__) || defined(_WIN32)

TEST_CASE("M4A files read through the system's decoder, their priming left out and each channel's tone in time and in place")
{
    struct Expected { const char* name; double rate; int channels; std::uint64_t frames; };
    for (const Expected& expected : {Expected{"tones-stereo-48000.m4a", 48000, 2, 24000}, Expected{"tones-5.1-48000.m4a", 48000, 6, 24000},
                                     Expected{"tones-stereo-44100-ffmpeg.m4a", 44100, 2, 22050}})
    {
        CAPTURE(expected.name);
        auto reader = files::openAudio(reference(expected.name));
        const auto& info = reader->info();
        CHECK(info.format == "M4A");
        CHECK(info.sampleRate == expected.rate);
        CHECK(info.channels == expected.channels);
        CHECK(info.bits == 0);
        REQUIRE(info.frames == expected.frames);

        const auto samples = pattern::readAll(*reader);
        for (int c = 0; c < info.channels; ++c)
        {
            CAPTURE(c);
            CHECK(pattern::toneMatch(samples[static_cast<std::size_t>(c)], c, expected.rate) > 20.0);
        }

        // Stretches out of order, each the same as where it sits in the whole.
        for (const std::uint64_t at : {15000u, 3u, 9000u, 1024u, 0u, 22000u})
        {
            CAPTURE(at);
            const auto part = pattern::stretch(*reader, at, 1000);
            CHECK(part.front().size() == std::min<std::uint64_t>(1000, expected.frames - at));
            CHECK(pattern::largestDifference(part, samples, at) < 1.0e-5f);
        }
    }
}

TEST_CASE("Apple Lossless reads sample for sample through the system's decoder")
{
    auto reader = files::openAudio(reference("alac24-stereo-48000.m4a"));
    const auto& info = reader->info();
    CHECK(info.format == "M4A");
    CHECK(info.sampleRate == 48000);
    CHECK(info.channels == 2);
    CHECK(info.bits == 24);
    REQUIRE(info.frames == pattern::referenceFrames);

    const auto samples = pattern::readAll(*reader);
    for (int c = 0; c < 2; ++c)
        for (std::uint64_t frame = 0; frame < pattern::referenceFrames; ++frame)
            if (samples[static_cast<std::size_t>(c)][frame] != pattern::read(frame, c, 24))
            {
                CAPTURE(c, frame);
                FAIL("a sample differs");
            }
}

TEST_CASE("AAC in ADTS, which says nothing of its priming, reads as the system decodes it, each channel's tone in place")
{
    auto reader = files::openAudio(reference("tones-stereo-48000.aac"));
    const auto& info = reader->info();
    CHECK(info.format == "AAC");
    CHECK(info.sampleRate == 48000);
    CHECK(info.channels == 2);
    REQUIRE(info.frames >= 24000);

    // Where the tones start depends on the decoder; it's the same for both channels, and not far in.
    const auto samples = pattern::readAll(*reader);
    std::size_t start[2]{};
    for (int c = 0; c < 2; ++c)
    {
        const auto& channel = samples[static_cast<std::size_t>(c)];
        start[c] = toneStart(channel, c, 48000, 4096, 20000);
        CAPTURE(c, start[c]);
        CHECK(start[c] < 4096);
        CHECK(pattern::toneMatch({channel.begin() + static_cast<std::ptrdiff_t>(start[c]),
                                  channel.begin() + static_cast<std::ptrdiff_t>(start[c] + 20000)}, c, 48000) > 20.0);
    }
    CHECK(start[0] == start[1]);
}

#endif

TEST_CASE("An MPEG-4 file's priming and length come from its edit list or iTunSMPB, and Apple Lossless' bits from its own box")
{
    // Apple's encoder writes iTunSMPB alone, and ffmpeg an edit list alone, its length in milliseconds.
    struct Expected { const char* name; double rate; std::uint64_t priming; std::uint64_t frames; int bits; };
    for (const Expected& expected : {Expected{"tones-stereo-48000.m4a", 48000, 2112, 24000, 0}, Expected{"tones-5.1-48000.m4a", 48000, 2112, 24000, 0},
                                     Expected{"tones-stereo-44100-ffmpeg.m4a", 44100, 1024, 22050, 0},
                                     Expected{"alac24-stereo-48000.m4a", 48000, 0, 1000, 24}})
    {
        CAPTURE(expected.name);
        std::error_code error;
        const auto file = detail::RandomAccessFile::open(reference(expected.name), error);
        REQUIRE(file != nullptr);
        const auto audio = detail::readMp4Audio(*file, expected.rate, error);
        CHECK(!error);
        CHECK(audio.priming == expected.priming);
        CHECK(audio.edited == (std::string_view(expected.name).find("ffmpeg") != std::string_view::npos));
        REQUIRE(audio.frames.has_value());
        CHECK(*audio.frames == expected.frames);
        CHECK(audio.losslessBits == expected.bits);
    }

    std::error_code error;
    const auto other = detail::RandomAccessFile::open(reference("pcm16-stereo-44100.wav"), error);
    REQUIRE(other != nullptr);
    const auto nothing = detail::readMp4Audio(*other, 44100, error);
    CHECK(!error);
    CHECK(nothing.priming == 0);
    CHECK(!nothing.frames.has_value());
}

TEST_CASE("CAF reads through the Mac's decoder, and where there's none it, M4A and AAC are said to be unsupported")
{
#if defined(__APPLE__)
    auto reader = files::openAudio(reference("pcm16-stereo-44100.caf"));
    CHECK(reader->info().format == "CAF");
    CHECK(reader->info().bits == 16);
    REQUIRE(reader->info().frames == pattern::referenceFrames);
    std::vector<std::vector<float>> expected(2);
    for (int c = 0; c < 2; ++c)
        for (std::uint64_t frame = 0; frame < pattern::referenceFrames; ++frame)
            expected[static_cast<std::size_t>(c)].push_back(pattern::read(frame, c, 16));
    CHECK(pattern::readAll(*reader) == expected);
#else
    std::vector<const char*> unsupported{"pcm16-stereo-44100.caf"};
    #if !defined(_WIN32)
    unsupported.insert(unsupported.end(), {"tones-stereo-48000.m4a", "alac24-stereo-48000.m4a", "tones-stereo-48000.aac"});
    #endif
    for (const char* name : unsupported)
    {
        CAPTURE(name);
        std::error_code error;
        CHECK(files::openAudio(reference(name), error) == nullptr);
        CHECK(error == AudioFileError::unsupported);
    }
#endif
}
