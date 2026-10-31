#include "Pattern.h"
#include "Scratch.h"

#include <symbolengine/core/Files.h>
#include <symbolengine/files/AudioFile.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

using namespace symbolengine;

namespace
{
// The pattern's samples for a file of these settings: whole numbers its bits hold exactly, or floats.
std::vector<std::vector<float>> patternFor(const AudioFileSettings& settings, std::uint64_t frames)
{
    std::vector<std::vector<float>> channels(static_cast<std::size_t>(settings.channels));
    for (int c = 0; c < settings.channels; ++c)
        for (std::uint64_t frame = 0; frame < frames; ++frame)
            channels[static_cast<std::size_t>(c)].push_back(settings.floatingPoint ? static_cast<float>(pattern::floating(frame, c))
                                                                                   : pattern::read(frame, c, settings.bits));
    return channels;
}

// Writes the samples in uneven pieces.
void writeInPieces(AudioFileWriter& writer, const std::vector<std::vector<float>>& channels)
{
    const auto frames = channels.front().size();
    std::vector<const float*> pointers(channels.size());
    for (std::size_t at = 0, piece = 1; at < frames; at += piece, piece = piece * 3 + 1)
    {
        piece = std::min(piece, frames - at);
        for (std::size_t c = 0; c < channels.size(); ++c)
            pointers[c] = channels[c].data() + at;
        writer.write(pointers.data(), piece);
    }
}

// What a whole-number format writes a float as, read back.
float writtenAs(float sample, int bits, const std::string& format)
{
    ScratchFolder folder;
    const auto file = folder.path / ("one." + format);
    const float* channels[1]{&sample};
    {
        auto writer = files::createAudio(file, {format == "wav" ? "WAV" : "AIFF", 48000, 1, bits, false, 0, {}});
        writer->write(channels, 1);
    }
    float back = 99.0f;
    float* out[1]{&back};
    files::openAudio(file)->read(0, out, 1);
    return back;
}
}

TEST_CASE("Every kind of WAV and AIFF written reads back sample for sample, in pieces of any size")
{
    ScratchFolder folder;
    constexpr std::uint64_t frames = 777;

    for (const std::string format : {"WAV", "AIFF"})
        for (const auto& [bits, floatingPoint] : {std::pair{8, false}, std::pair{16, false}, std::pair{24, false}, std::pair{32, false},
                                                  std::pair{32, true}, std::pair{64, true}})
            for (const int channels : {1, 2, 6})
            {
                CAPTURE(format, bits, floatingPoint, channels);
                const AudioFileSettings settings{format, 44100, channels, bits, floatingPoint, 0, {}};
                const auto file = folder.path / ("trip-" + std::to_string(bits) + (floatingPoint ? "f" : "") + "-" +
                                                 std::to_string(channels) + "." + format);
                const auto samples = patternFor(settings, frames);

                auto writer = files::createAudio(file, settings);
                writeInPieces(*writer, samples);
                CHECK(writer->frames() == frames);
                writer->finish();

                auto reader = files::openAudio(file);
                const auto& info = reader->info();
                CHECK(info.format == format);
                CHECK(info.sampleRate == 44100);
                CHECK(info.channels == channels);
                CHECK(info.bits == bits);
                CHECK(info.floatingPoint == floatingPoint);
                REQUIRE(info.frames == frames);
                CHECK(pattern::readAll(*reader, 100) == samples);
            }
}

TEST_CASE("Whole-number formats write a float as the nearest whole number, clipped to their range")
{
    for (const std::string format : {"wav", "aiff"})
    {
        CAPTURE(format);
        CHECK(writtenAs(1.0f, 16, format) == 32767.0f / 32768.0f);
        CHECK(writtenAs(2.0f, 16, format) == 32767.0f / 32768.0f);
        CHECK(writtenAs(-1.0f, 16, format) == -1.0f);
        CHECK(writtenAs(-2.0f, 16, format) == -1.0f);
        CHECK(writtenAs(std::numeric_limits<float>::quiet_NaN(), 16, format) == 0.0f);
        CHECK(writtenAs(std::numeric_limits<float>::infinity(), 24, format) == 8388607.0f / 8388608.0f);
        CHECK(writtenAs(0.4f / 32768.0f, 16, format) == 0.0f);
        CHECK(writtenAs(0.6f / 32768.0f, 16, format) == 1.0f / 32768.0f);
        CHECK(writtenAs(-0.6f / 32768.0f, 16, format) == -1.0f / 32768.0f);
        CHECK(writtenAs(1.0f, 32, format) == 1.0f);   // 2147483647 / 2^31, as near to 1 as a float gets
        CHECK(writtenAs(0.5f, 8, format) == 0.5f);
        CHECK(writtenAs(-1.0f, 8, format) == -1.0f);
    }
}

TEST_CASE("Float formats keep every float as it is, past full scale too")
{
    ScratchFolder folder;
    const float samples[4]{1.5f, -3.25f, 1.0e-30f, -0.0f};
    const float* channels[1]{samples};

    for (const std::string format : {"WAV", "AIFF"})
        for (const int bits : {32, 64})
        {
            CAPTURE(format, bits);
            const auto file = folder.path / ("float." + format);
            {
                auto writer = files::createAudio(file, {format, 48000, 1, bits, true, 0, {}});
                writer->write(channels, 4);
            }
            CHECK(pattern::readAll(*files::openAudio(file))[0] == std::vector<float>(samples, samples + 4));
        }
}

TEST_CASE("Chunks written read back in their order, odd lengths and all, those that lay out the file left out")
{
    ScratchFolder folder;
    const float silence[2]{};
    const float* channels[1]{silence};

    const std::vector<AudioChunk> given{{"bext", std::string(602, 'x')}, {"iXML", "<BWFXML/>"}, {"LIST", "odd"},
                                        {"data", "never"}, {"fmt ", "never"}, {"COMM", "never"}, {"JUNK", "never"}, {"toolong", "never"}};

    for (const std::string format : {"WAV", "AIFF"})
    {
        CAPTURE(format);
        const auto file = folder.path / ("chunks." + format);
        {
            auto writer = files::createAudio(file, {format, 48000, 1, 16, false, 0, given});
            writer->write(channels, 2);
        }
        const auto reader = files::openAudio(file);
        const std::vector<AudioChunk> expected{{"bext", std::string(602, 'x')}, {"iXML", "<BWFXML/>"}, {"LIST", "odd"}};
        CHECK(reader->info().chunks == expected);
        CHECK(reader->info().frames == 2);
    }
}

TEST_CASE("Reading stops at the end of the file, a stretch past it reading nothing")
{
    auto reader = files::openAudio(pattern::reference("pcm16-stereo-44100.wav"));
    std::vector<float> left(100), right(100);
    float* out[2]{left.data(), right.data()};

    CHECK(reader->read(950, out, 100) == 50);
    CHECK(left[49] == pattern::read(999, 0, 16));
    CHECK(reader->read(1000, out, 100) == 0);
    CHECK(reader->read(5000, out, 100) == 0);
    CHECK(reader->read(10, out, 0) == 0);
    CHECK(reader->read(10, out, 1) == 1);
    CHECK(right[0] == pattern::read(10, 1, 16));
}

TEST_CASE("A writer let go of without finishing completes its file")
{
    ScratchFolder folder;
    const auto file = folder.path / "unfinished.wav";
    const float samples[3]{0.5f, 0.25f, 0.125f};
    const float* channels[1]{samples};
    {
        auto writer = files::createAudio(file, {"WAV", 48000, 1, 24, false, 0, {}});
        writer->write(channels, 3);
    }
    CHECK(pattern::readAll(*files::openAudio(file))[0] == std::vector<float>(samples, samples + 3));
}

TEST_CASE("Files that can't be read, and settings that can't be written, say why")
{
    ScratchFolder folder;
    std::error_code error;

    CHECK(files::openAudio(folder.path / "missing.wav", error) == nullptr);
    CHECK(error == std::errc::no_such_file_or_directory);
    CHECK_THROWS_AS(files::openAudio(folder.path / "missing.wav"), std::filesystem::filesystem_error);

    files::writeSafely(folder.path / "text.wav", "This is a letter, not a sound.");
    CHECK(files::openAudio(folder.path / "text.wav", error) == nullptr);
    CHECK(error == AudioFileError::notAudio);
    CHECK(error.message() == "The file isn't audio of a kind that can be read.");

    files::writeSafely(folder.path / "short.wav", "RIFF");
    CHECK(files::openAudio(folder.path / "short.wav", error) == nullptr);
    CHECK(error == AudioFileError::notAudio);

    // A WAV with no fmt chunk.
    files::writeSafely(folder.path / "nofmt.wav", std::string("RIFF\x0c\0\0\0WAVEdata\0\0\0\0", 20));
    CHECK(files::openAudio(folder.path / "nofmt.wav", error) == nullptr);
    CHECK(error == AudioFileError::damaged);

    const auto refused = [&](AudioFileSettings settings)
    {
        const auto writer = files::createAudio(folder.path / "refused.wav", settings, error);
        return writer == nullptr && error == AudioFileError::badSettings;
    };
    CHECK(refused({"WAV", 48000, 2, 12, false, 0, {}}));
    CHECK(refused({"WAV", 48000, 2, 16, true, 0, {}}));
    CHECK(refused({"WAV", 48000, 0, 16, false, 0, {}}));
    CHECK(refused({"WAV", 0, 2, 16, false, 0, {}}));
    CHECK(refused({"WAV", std::nan(""), 2, 16, false, 0, {}}));
    CHECK(refused({"MP3", 48000, 2, 16, false, 0, {}}));
    CHECK(!std::filesystem::exists(folder.path / "refused.wav"));

    CHECK(files::createAudio(folder.path / "no folder" / "x.wav", {"WAV", 48000, 2, 16, false, 0, {}}, error) == nullptr);
    CHECK(error == std::errc::no_such_file_or_directory);
}

TEST_CASE("A finished writer writes nothing more")
{
    ScratchFolder folder;
    const float sample = 0.5f;
    const float* channels[1]{&sample};
    auto writer = files::createAudio(folder.path / "done.wav", {"WAV", 48000, 1, 16, false, 0, {}});
    writer->write(channels, 1);
    writer->finish();
    std::error_code error;
    writer->write(channels, 1, error);
    CHECK(error);
    writer->flush(error);
    CHECK(error);
    CHECK(files::openAudio(folder.path / "done.wav")->info().frames == 1);
}

TEST_CASE("Settings refused, by the format or by its encoder, leave a file of that name as it was")
{
    ScratchFolder folder;
    const auto file = folder.path / "take";
    const std::string kept = "what the user had";

    // WAV's bits and rate are refused from what the format holds; FLAC's rate past 65535 unless a multiple of ten, and
    // less than a hertz for Opus and Ogg Vorbis, by their encoders, which are given the file first.
    for (const AudioFileSettings& settings :
         {AudioFileSettings{"WAV", 48000, 2, 12, false, 0, {}}, AudioFileSettings{"WAV", 0.2, 2, 16, false, 0, {}},
          AudioFileSettings{"FLAC", 88201, 2, 24, false, 0, {}}, AudioFileSettings{"Opus", 0.5, 2, 0, false, 0, {}},
          AudioFileSettings{"Ogg Vorbis", 0.5, 1, 0, false, 0, {}}})
    {
        CAPTURE(settings.format, settings.sampleRate);
        files::writeSafely(file, kept);
        std::error_code error;
        CHECK(files::createAudio(file, settings, error) == nullptr);
        CHECK(error == AudioFileError::badSettings);
        CHECK(files::read(file) == kept);

        // And none is left where there was none.
        std::error_code ignored;
        CHECK(files::createAudio(folder.path / "new", settings, ignored) == nullptr);
        CHECK_FALSE(std::filesystem::exists(folder.path / "new"));
    }
}

TEST_CASE("A file written over a longer one holds only what was written")
{
    ScratchFolder folder;
    const auto file = folder.path / "take";
    const std::vector<float> samples(4800, 0.25f);
    const float* channels[] = {samples.data()};

    for (const AudioFileSettings& settings :
         {AudioFileSettings{"WAV", 48000, 1, 16, false, 0, {}}, AudioFileSettings{"AIFF", 48000, 1, 16, false, 0, {}},
          AudioFileSettings{"FLAC", 48000, 1, 16, false, 0, {}},
          AudioFileSettings{"Ogg Vorbis", 48000, 1, 0, false, 0, {}},
          AudioFileSettings{"Opus", 48000, 1, 0, false, 0, {}}})
    {
        CAPTURE(settings.format);
        const std::string longer(1000000, 'x');
        files::writeSafely(file, longer);
        {
            auto writer = files::createAudio(file, settings);
            REQUIRE(writer != nullptr);
            writer->write(channels, samples.size());
            writer->finish();
        }
        CHECK(std::filesystem::file_size(file) < longer.size());
        CHECK(files::openAudio(file)->info().frames == samples.size());
    }
}
