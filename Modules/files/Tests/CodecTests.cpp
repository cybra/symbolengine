#include "Pattern.h"
#include "Scratch.h"

#include <symbolengine/core/Files.h>
#include <symbolengine/files/AudioFile.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <string>

using namespace symbolengine;
using pattern::reference;

namespace
{
using Tags = std::vector<std::pair<std::string, std::string>>;

std::vector<std::vector<float>> tones(int channels, double rate, std::uint64_t frames)
{
    std::vector<std::vector<float>> samples(static_cast<std::size_t>(channels));
    for (int c = 0; c < channels; ++c)
        for (std::uint64_t frame = 0; frame < frames; ++frame)
            samples[static_cast<std::size_t>(c)].push_back(static_cast<float>(pattern::tone(frame, c, rate)));
    return samples;
}

void writeAll(AudioFileWriter& writer, const std::vector<std::vector<float>>& samples, std::size_t piece = 1000)
{
    std::vector<const float*> pointers(samples.size());
    for (std::size_t at = 0; at < samples.front().size(); at += piece)
    {
        for (std::size_t c = 0; c < samples.size(); ++c)
            pointers[c] = samples[c].data() + at;
        writer.write(pointers.data(), std::min(piece, samples.front().size() - at));
    }
}

// The pattern's whole numbers for a file of these bits and channels.
std::vector<std::vector<float>> wholePattern(int channels, int bits, std::uint64_t frames)
{
    std::vector<std::vector<float>> samples(static_cast<std::size_t>(channels));
    for (int c = 0; c < channels; ++c)
        for (std::uint64_t frame = 0; frame < frames; ++frame)
            samples[static_cast<std::size_t>(c)].push_back(pattern::read(frame, c, bits));
    return samples;
}
}

TEST_CASE("FLAC files written by other programs read sample for sample, with their tags")
{
    struct Expected { const char* name; double rate; int channels; int bits; };
    for (const Expected& expected : {Expected{"pcm16-stereo-44100.flac", 44100, 2, 16}, Expected{"pcm24-5.1-48000.flac", 48000, 6, 24},
                                     Expected{"tagged-pcm24-mono-48000.flac", 48000, 1, 24}})
    {
        CAPTURE(expected.name);
        auto reader = files::openAudio(reference(expected.name));
        const auto& info = reader->info();
        CHECK(info.format == "FLAC");
        CHECK(info.sampleRate == expected.rate);
        CHECK(info.channels == expected.channels);
        CHECK(info.bits == expected.bits);
        CHECK(!info.floatingPoint);
        REQUIRE(info.frames == pattern::referenceFrames);
        CHECK(pattern::readAll(*reader) == wholePattern(expected.channels, expected.bits, pattern::referenceFrames));
    }

    const auto tagged = files::openAudio(reference("tagged-pcm24-mono-48000.flac"));
    CHECK(tagged->info().tags == Tags{{"TITLE", "A take for the tests"}, {"ARTIST", "SymbolEngine"}, {"DATE", "2026"},
                                       {"WAVEFORMATEXTENSIBLE_CHANNEL_MASK", "0x0004"}});
}

TEST_CASE("FLAC written reads back sample for sample, with its tags, from anywhere in it")
{
    ScratchFolder folder;
    const Tags tags{{"TITLE", "Écoute"}, {"ARTIST", "SymbolEngine"}, {"COMMENT", "a=b"}};

    for (const int bits : {16, 24})
        for (const int channels : {1, 2, 6, 8})
            for (const int compression : {0, 5, 8})
            {
                CAPTURE(bits, channels, compression);
                const auto file = folder.path / "written.flac";
                const auto samples = wholePattern(channels, bits, 30000);
                AudioFileSettings settings{"FLAC", 48000, channels, bits, false, 0, {}, tags};
                settings.compression = compression;
                {
                    auto writer = files::createAudio(file, settings);
                    writeAll(*writer, samples, 777);
                }

                auto reader = files::openAudio(file);
                CHECK(reader->info().format == "FLAC");
                CHECK(reader->info().bits == bits);
                CHECK(reader->info().tags == tags);
                REQUIRE(reader->info().frames == 30000);
                CHECK(pattern::readAll(*reader, 4321) == samples);

                // Stretches out of order, each the same as where it sits in the whole.
                for (const std::uint64_t from : {29000u, 5u, 17000u, 4096u, 0u, 29999u})
                {
                    const auto part = pattern::stretch(*reader, from, 900);
                    for (int c = 0; c < channels; ++c)
                        CHECK(std::equal(part[static_cast<std::size_t>(c)].begin(), part[static_cast<std::size_t>(c)].end(),
                                         samples[static_cast<std::size_t>(c)].begin() + static_cast<std::ptrdiff_t>(from)));
                }
            }
}

TEST_CASE("Opus and Ogg Vorbis files written by other programs read each channel's tone in time and in place")
{
    struct Expected { const char* name; const char* format; double rate; int channels; std::uint64_t frames; double written; bool sameTone; };
    for (const Expected& expected : {Expected{"tones-stereo-48000.opus", "Opus", 48000, 2, 24000, 48000, false},
                                     Expected{"tones-5.1-48000.opus", "Opus", 48000, 6, 24000, 48000, false},
                                     Expected{"tone-stereo-44100.ogg", "Ogg Vorbis", 44100, 2, 22050, 44100, true}})
    {
        CAPTURE(expected.name);
        auto reader = files::openAudio(reference(expected.name));
        const auto& info = reader->info();
        CHECK(info.format == expected.format);
        CHECK(info.sampleRate == expected.rate);
        CHECK(info.channels == expected.channels);
        CHECK(info.bits == 0);

        // ffmpeg's own Vorbis encoder leaves its last packet's few extra samples marked as audio.
        CHECK(info.frames >= expected.frames);
        CHECK(info.frames <= expected.frames + 64);

        const auto samples = pattern::readAll(*reader);
        for (int c = 0; c < info.channels; ++c)
        {
            CAPTURE(c);
            const auto& channel = samples[static_cast<std::size_t>(c)];
            CHECK(pattern::toneMatch(std::vector<float>(channel.begin(), channel.begin() + static_cast<std::ptrdiff_t>(expected.frames)),
                                     expected.sameTone ? 0 : c, expected.written) > 20.0);
        }
    }

    const Tags tags{{"title", "A take for the tests"}, {"artist", "SymbolEngine"}};
    for (const char* name : {"tones-stereo-48000.opus", "tone-stereo-44100.ogg"})
    {
        CAPTURE(name);
        auto found = files::openAudio(reference(name))->info().tags;
        std::erase_if(found, [](const auto& tag) { return tag.first == "encoder" || tag.first == "ENCODER"; });
        CHECK(found == tags);
    }
}

TEST_CASE("Opus and Ogg Vorbis written read back their length, each channel's tone and their tags")
{
    ScratchFolder folder;
    const Tags tags{{"TITLE", "Écoute"}, {"ARTIST", "SymbolEngine"}};

    for (const std::string format : {"Opus", "Ogg Vorbis"})
        for (const int channels : {1, 2, 6, 8})
        {
            CAPTURE(format, channels);
            const auto file = folder.path / ("tones." + std::string(format == "Opus" ? "opus" : "ogg"));
            const auto samples = tones(channels, 48000, 48000);
            AudioFileSettings settings{format, 48000, channels, 0, false, 0, {}, tags};
            settings.bitRate = 96000 * channels;
            settings.quality = 0.6;
            {
                auto writer = files::createAudio(file, settings);
                writeAll(*writer, samples, 1234);
                writer->finish();
            }

            auto reader = files::openAudio(file);
            const auto& info = reader->info();
            CHECK(info.format == format);
            CHECK(info.channels == channels);
            CHECK(info.frames == 48000);
            auto found = info.tags;
            std::erase_if(found, [](const auto& tag) { return tag.first == "ENCODER"; });
            CHECK(found == tags);

            // A channel out of place scores about -3 dB. libvorbis' 5.1 codes the LFE, the fourth channel, in a narrow band
            // of its own, and ffmpeg's decoder hears the same.
            const auto back = pattern::readAll(*reader, 999);
            for (int c = 0; c < channels; ++c)
            {
                CAPTURE(c);
                CHECK(pattern::toneMatch(back[static_cast<std::size_t>(c)], c, 48000) > (format == "Ogg Vorbis" && channels == 6 && c == 3 ? 10.0 : 20.0));
            }

            // A stretch read on its own matches the same stretch read from the start.
            const auto part = pattern::stretch(*reader, 30000, 4800);
            for (int c = 0; c < channels; ++c)
            {
                float most = 0.0f;
                for (std::size_t i = 0; i < part[static_cast<std::size_t>(c)].size(); ++i)
                    most = std::max(most, std::abs(part[static_cast<std::size_t>(c)][i] - back[static_cast<std::size_t>(c)][30000 + i]));
                CHECK(most < 1.0e-5f);
            }
        }
}

TEST_CASE("Opus written at another rate plays at 48 kHz, its length in time kept")
{
    ScratchFolder folder;
    const auto file = folder.path / "44100.opus";
    {
        auto writer = files::createAudio(file, {"Opus", 44100, 1, 0, false, 0, {}});
        writeAll(*writer, tones(1, 44100, 44100));
    }
    const auto reader = files::openAudio(file);
    CHECK(reader->info().sampleRate == 48000);
    CHECK(reader->info().frames >= 47990);
    CHECK(reader->info().frames <= 48010);
}

TEST_CASE("Settings FLAC can't hold are refused, and an Ogg FLAC file is said to be one that can't be read")
{
    ScratchFolder folder;
    std::error_code error;
    for (const AudioFileSettings& settings : {AudioFileSettings{"FLAC", 48000, 2, 32, false, 0, {}}, AudioFileSettings{"FLAC", 48000, 2, 32, true, 0, {}},
                                              AudioFileSettings{"FLAC", 48000, 9, 16, false, 0, {}}, AudioFileSettings{"FLAC", 44100.5, 2, 16, false, 0, {}},
                                              AudioFileSettings{"Opus", 48000, 256, 0, false, 0, {}}})
    {
        CAPTURE(settings.format, settings.bits, settings.channels);
        CHECK(files::createAudio(folder.path / "refused", settings, error) == nullptr);
        CHECK(error == AudioFileError::badSettings);
    }

    // An Ogg page whose first packet is FLAC's.
    std::string page = "OggS";
    page += std::string("\x00\x02", 2) + std::string(20, '\0') + std::string("\x01\x0d", 2) + "\x7f" "FLAC" + std::string(8, '\0');
    files::writeSafely(folder.path / "ogg.flac", page);
    CHECK(files::openAudio(folder.path / "ogg.flac", error) == nullptr);
    CHECK(error == AudioFileError::unsupported);
}

TEST_CASE("Opus files joined into one read in stretches of any size, never past the room given and never short")
{
    ScratchFolder folder;
    constexpr std::uint64_t each = 24000;
    const auto made = [&](const char* name, int channels) {
        const auto file = folder.path / name;
        auto writer = files::createAudio(file, {"Opus", 48000, channels, 0, false, 0, {}});
        writeAll(*writer, tones(channels, 48000, each));
        writer->finish();
        return files::read(file);
    };
    const auto stereo = made("stereo.opus", 2);
    const auto mono = made("mono.opus", 1);
    files::writeSafely(folder.path / "stereo then mono.opus", stereo + mono);
    files::writeSafely(folder.path / "mono then stereo.opus", mono + stereo);

    for (const char* name : {"stereo then mono.opus", "mono then stereo.opus"})
        for (const std::size_t piece : {std::size_t{1}, std::size_t{480}, std::size_t{1000}, std::size_t{7000}})
        {
            CAPTURE(name, piece);
            const auto reader = files::openAudio(folder.path / name);
            const auto channels = static_cast<std::size_t>(reader->info().channels);
            const auto frames = reader->info().frames;
            REQUIRE(frames == 2 * each);

            // Each buffer has room past what a read asks for, which must stay as it was.
            constexpr float untouched = 1234.5f;
            constexpr std::size_t past = 12000;
            std::vector<std::vector<float>> room(channels, std::vector<float>(piece + past, untouched));
            std::vector<float*> pointers;
            for (auto& channel : room)
                pointers.push_back(channel.data());

            for (std::uint64_t at = 0; at < frames;)
            {
                const auto got = reader->read(at, pointers.data(), piece);
                REQUIRE(got == std::min<std::uint64_t>(piece, frames - at));
                for (const auto& channel : room)
                    REQUIRE(std::all_of(channel.begin() + static_cast<std::ptrdiff_t>(piece), channel.end(),
                                        [](float sample) { return sample == untouched; }));
                at += got;
            }
        }

    // What is read is each link's own audio, and a channel a link hasn't is silent.
    const auto reader = files::openAudio(folder.path / "stereo then mono.opus");
    const auto all = pattern::readAll(*reader, 999);
    const auto first = [&](std::size_t c) {
        return std::vector<float>(all[c].begin(), all[c].begin() + static_cast<std::ptrdiff_t>(each));
    };
    const auto second = [&](std::size_t c) {
        return std::vector<float>(all[c].begin() + static_cast<std::ptrdiff_t>(each), all[c].end());
    };
    CHECK(pattern::toneMatch(first(0), 0, 48000) > 20.0);
    CHECK(pattern::toneMatch(first(1), 1, 48000) > 20.0);
    CHECK(pattern::toneMatch(second(0), 0, 48000) > 20.0);
    const auto right = second(1);
    CHECK(std::all_of(right.begin(), right.end(), [](float sample) { return sample == 0.0f; }));

    // A stretch read on its own, in the second link and across the join, matches the same one read from the start.
    for (const std::uint64_t from : {each + 6000, each - 2400})
    {
        CAPTURE(from);
        const auto part = pattern::stretch(*reader, from, 4800);
        for (std::size_t c = 0; c < all.size(); ++c)
        {
            float most = 0.0f;
            for (std::size_t i = 0; i < part[c].size(); ++i)
                most = std::max(most, std::abs(part[c][i] - all[c][static_cast<std::size_t>(from) + i]));
            CHECK(most < 1.0e-5f);
        }
    }
}

TEST_CASE("Every number of channels a format is said to take reads back")
{
    ScratchFolder folder;
    struct Kind
    {
        const char* format;
        const char* name;
        int channels;
    };
    for (const Kind& kind : {Kind{"Opus", "most.opus", 8}, Kind{"Ogg Vorbis", "nine.ogg", 9},
                             Kind{"Ogg Vorbis", "most.ogg", 255}, Kind{"FLAC", "most.flac", 8}})
    {
        CAPTURE(kind.format, kind.channels);
        const auto file = folder.path / kind.name;
        AudioFileSettings settings{kind.format, 48000, kind.channels, 24, false, 0, {}};
        {
            auto writer = files::createAudio(file, settings);
            writeAll(*writer, tones(kind.channels, 48000, 4800));
            writer->finish();
        }
        std::error_code error;
        const auto reader = files::openAudio(file, error);
        REQUIRE(reader != nullptr);
        CHECK(reader->info().channels == kind.channels);
        CHECK(reader->info().frames == 4800);
    }

    std::error_code error;
    CHECK(files::createAudio(folder.path / "nine.opus", {"Opus", 48000, 9, 0, false, 0, {}}, error) == nullptr);
    CHECK(error == AudioFileError::badSettings);
}

TEST_CASE("A FLAC that doesn't say its length, as one piped or stopped short, is measured and reads whole")
{
    ScratchFolder folder;
    const auto file = folder.path / "written.flac";
    const auto samples = wholePattern(2, 24, 30000);
    {
        auto writer = files::createAudio(file, {"FLAC", 48000, 2, 24, false, 0, {}, {{"TITLE", "Écoute"}}});
        writeAll(*writer, samples, 777);
    }

    // Past "fLaC", the block's header and the first ten bytes of STREAMINFO: 20 bits of rate, 3 of channels, 5 of bits
    // and 36 of length.
    auto bytes = files::read(file);
    REQUIRE(bytes.size() > 26);
    bytes[21] = static_cast<char>(static_cast<unsigned char>(bytes[21]) & 0xF0);
    bytes[22] = bytes[23] = bytes[24] = bytes[25] = 0;

    const auto unsaid = folder.path / "unsaid.flac";
    files::writeSafely(unsaid, bytes);
    {
        auto reader = files::openAudio(unsaid);
        CHECK(reader->info().sampleRate == 48000);
        CHECK(reader->info().channels == 2);
        CHECK(reader->info().tags.size() == 1);
        REQUIRE(reader->info().frames == 30000);
        CHECK(pattern::readAll(*reader, 4321) == samples);

        for (const std::uint64_t from : {29000u, 5u, 17000u, 4096u, 0u, 29999u})
        {
            CAPTURE(from);
            const auto part = pattern::stretch(*reader, from, 900);
            for (std::size_t c = 0; c < part.size(); ++c)
                CHECK(
                    std::equal(part[c].begin(), part[c].end(), samples[c].begin() + static_cast<std::ptrdiff_t>(from)));
        }
    }

    // Stopped partway, as a crash leaves one: its whole frames, as far as they go.
    const auto cut = folder.path / "cut.flac";
    files::writeSafely(cut, bytes.substr(0, bytes.size() * 2 / 3));
    auto reader = files::openAudio(cut);
    const auto frames = reader->info().frames;
    CHECK(frames > 10000);
    CHECK(frames < 30000);
    const auto part = pattern::readAll(*reader, 999);
    REQUIRE(part.front().size() == frames);
    for (std::size_t c = 0; c < part.size(); ++c)
        CHECK(std::equal(part[c].begin(), part[c].end(), samples[c].begin()));
}

TEST_CASE("A FLAC whose rate is nothing is damaged")
{
    ScratchFolder folder;
    const auto file = folder.path / "written.flac";
    {
        auto writer = files::createAudio(file, {"FLAC", 48000, 2, 16, false, 0, {}});
        writeAll(*writer, wholePattern(2, 16, 4096));
    }
    auto bytes = files::read(file);
    bytes[18] = bytes[19] = 0;
    bytes[20] = static_cast<char>(static_cast<unsigned char>(bytes[20]) & 0x0F);
    files::writeSafely(file, bytes);

    std::error_code error;
    CHECK(files::openAudio(file, error) == nullptr);
    CHECK(error == AudioFileError::damaged);
}
