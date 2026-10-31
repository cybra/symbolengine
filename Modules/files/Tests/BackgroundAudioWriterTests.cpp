#include "Pattern.h"
#include "Scratch.h"

#include <symbolengine/core/Files.h>
#include <symbolengine/files/BackgroundAudioWriter.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace symbolengine;
using namespace std::chrono_literals;

namespace
{
std::uint32_t little32(const std::string& bytes, std::size_t at)
{
    std::uint32_t value = 0;
    for (int i = 3; i >= 0; --i)
        value = value << 8 | static_cast<unsigned char>(bytes[at + static_cast<std::size_t>(i)]);
    return value;
}

// Waits up to five seconds for what another thread does, saying whether it came.
template <typename Condition>
bool soon(Condition condition)
{
    const auto until = std::chrono::steady_clock::now() + 5s;
    while (!condition())
    {
        if (std::chrono::steady_clock::now() > until)
            return false;
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

// A writer that keeps nothing, counting the stretches it is given and its flushes, and failing from its second stretch
// on when told to, as a full disk would.
class CountingWriter final : public AudioFileWriter
{
public:
    explicit CountingWriter(bool failing) : fails(failing) { written = {"test", 48000, 1, 32, true, 0, {}}; }

    std::atomic<int> stretches{0}, flushes{0};
    std::atomic<std::uint64_t> framesAtFlush{0};

private:
    void writeFrames(const float* const*, std::size_t, std::error_code& error) override
    {
        if (++stretches > 1 && fails)
            error = std::make_error_code(std::errc::no_space_on_device);
    }

    void flushFile(std::error_code&) override
    {
        framesAtFlush = count;
        ++flushes;
    }

    void finishFile(std::error_code&) override {}

    const bool fails;
};
}

TEST_CASE("Samples added on one thread reach the file on another whole and in order, and are given as written")
{
    ScratchFolder folder;
    const auto file = folder.path / "background.wav";
    constexpr std::size_t total = 200'000;
    std::vector<float> left(total), right(total);
    for (std::size_t i = 0; i < total; ++i)
    {
        left[i] = pattern::read(i, 0, 24);
        right[i] = pattern::read(i, 1, 24);
    }

    std::vector<float> givenLeft, givenRight;
    {
        BackgroundAudioWriter writer(files::createAudio(file, {"WAV", 48000, 2, 24, false, 0, {}}), 1000, 1s,
                                     [&](const float* const* channels, std::size_t frames)
                                     {
                                         givenLeft.insert(givenLeft.end(), channels[0], channels[0] + frames);
                                         givenRight.insert(givenRight.end(), channels[1], channels[1] + frames);
                                     });

        // Stretches of every size up to 700, which the buffer often has no room for, offered again until it has.
        std::thread adding(
            [&]
            {
                for (std::size_t at = 0, size = 1; at < total; size = size % 700 + 1)
                {
                    const auto frames = std::min(size, total - at);
                    const float* channels[2]{left.data() + at, right.data() + at};
                    if (writer.write(channels, frames))
                        at += frames;
                    else
                        std::this_thread::yield();
                }
            });
        adding.join();
        writer.finish();
    }

    const auto back = pattern::readAll(*files::openAudio(file));
    CHECK(back[0] == left);
    CHECK(back[1] == right);
    CHECK(givenLeft == left);
    CHECK(givenRight == right);
}

TEST_CASE("Each stretch is given as the file holds it: whole numbers as they read back, and floats as they are")
{
    ScratchFolder folder;
    std::vector<float> samples(4800);
    for (std::size_t i = 0; i < samples.size(); ++i)
        samples[i] = static_cast<float>(0.7 * std::sin(0.01 * static_cast<double>(i)));
    const float* channels[1]{samples.data()};

    for (const auto& settings : {AudioFileSettings{"WAV", 48000, 1, 16, false}, AudioFileSettings{"AIFF", 48000, 1, 24, false},
                                 AudioFileSettings{"FLAC", 48000, 1, 16, false}, AudioFileSettings{"WAV", 48000, 1, 32, true}})
    {
        const auto file = folder.path / ("given " + settings.format + std::to_string(settings.bits));
        std::vector<float> given;
        {
            BackgroundAudioWriter writer(files::createAudio(file, settings), samples.size(), 1s,
                                         [&](const float* const* c, std::size_t frames) { given.insert(given.end(), c[0], c[0] + frames); });
            CHECK(writer.write(channels, samples.size()));
        }

        INFO(settings.format << " " << settings.bits);
        CHECK(given == pattern::readAll(*files::openAudio(file))[0]);
        CHECK((given == samples) == settings.floatingPoint);
    }
}

TEST_CASE("Samples the buffer has no room for are refused, none of them added")
{
    ScratchFolder folder;
    const auto file = folder.path / "full.wav";
    const std::vector<float> samples(101, 0.5f);
    const float* channels[1]{samples.data()};

    BackgroundAudioWriter writer(files::createAudio(file, {"WAV", 48000, 1, 16, false, 0, {}}), 100);
    CHECK(!writer.write(channels, 101));
    CHECK(writer.write(channels, 100));
    writer.finish();
    CHECK(files::openAudio(file)->info().frames == 100);
}

TEST_CASE("While samples come, the header is rewritten every interval, so a file stopped by a crash reads to then")
{
    ScratchFolder folder;
    const auto file = folder.path / "flushed.wav";
    const std::vector<float> samples(4800, 0.25f);
    const float* channels[1]{samples.data()};

    BackgroundAudioWriter writer(files::createAudio(file, {"WAV", 48000, 1, 16, false, 0, {}}), 48000, 20ms);
    CHECK(writer.write(channels, samples.size()));

    // The sizes, which nothing but a flush writes before the file is finished.
    CHECK(soon(
        [&]
        {
            const auto bytes = files::read(file);
            return bytes.size() == 80 + 9600 && little32(bytes, 4) == bytes.size() - 8 && little32(bytes, 76) == 9600;
        }));

    std::filesystem::copy_file(file, folder.path / "crashed.wav");
    CHECK(files::openAudio(folder.path / "crashed.wav")->info().frames == 4800);
    writer.finish();
}

TEST_CASE("A file is flushed only when something has been written since it last was")
{
    auto counting = std::make_unique<CountingWriter>(false);
    auto& file = *counting;
    const float samples[100]{};
    const float* channels[1]{samples};

    BackgroundAudioWriter writer(std::move(counting), 1000, 10ms);
    CHECK(writer.write(channels, 100));
    CHECK(soon([&] { return file.flushes == 1; }));
    std::this_thread::sleep_for(100ms);
    CHECK(file.flushes == 1);
    CHECK(file.framesAtFlush == 100);

    CHECK(writer.write(channels, 100));
    CHECK(soon([&] { return file.flushes == 2 && file.framesAtFlush == 200; }));
}

TEST_CASE("Writing that goes wrong stops the writing, refuses more samples, and is said when finishing")
{
    const float samples[100]{};
    const float* channels[1]{samples};

    for (const auto throwing : {false, true})
    {
        auto failing = std::make_unique<CountingWriter>(true);
        auto& file = *failing;
        BackgroundAudioWriter writer(std::move(failing), 1000);

        CHECK(writer.write(channels, 100));
        CHECK(soon([&] { return file.stretches == 1; }));
        CHECK(!writer.failed());

        CHECK(writer.write(channels, 100));
        CHECK(soon([&] { return writer.failed(); }));
        CHECK(!writer.write(channels, 1));

        if (throwing)
        {
            CHECK_THROWS_AS(writer.finish(), std::filesystem::filesystem_error);
        }
        else
        {
            std::error_code error;
            writer.finish(error);
            CHECK(error == std::errc::no_space_on_device);
        }
        CHECK(file.stretches == 2);
    }
}

TEST_CASE("Letting go of the writer writes what is waiting and finishes the file, as finishing again does nothing")
{
    ScratchFolder folder;
    const std::vector<float> samples(500, 0.25f);
    const float* channels[1]{samples.data()};

    {
        BackgroundAudioWriter writer(files::createAudio(folder.path / "let go.wav", {"WAV", 48000, 1, 16, false, 0, {}}), 1000);
        CHECK(writer.write(channels, 500));
    }
    CHECK(little32(files::read(folder.path / "let go.wav"), 76) == 1000);

    BackgroundAudioWriter writer(files::createAudio(folder.path / "twice.wav", {"WAV", 48000, 1, 16, false, 0, {}}), 1000);
    CHECK(writer.write(channels, 500));
    writer.finish();
    CHECK_NOTHROW(writer.finish());
    CHECK(little32(files::read(folder.path / "twice.wav"), 76) == 1000);
}

namespace
{
// A writer that takes so many frames, then fails as a full disk does.
class FullAfter final : public AudioFileWriter
{
public:
    explicit FullAfter(std::uint64_t most) : room(most) { written = {"test", 48000, 1, 32, true, 0, {}}; }

private:
    void writeFrames(const float* const*, std::size_t frames, std::error_code& error) override
    {
        if (frames > room)
            error = std::make_error_code(std::errc::no_space_on_device);
        else
            room -= frames;
    }

    void finishFile(std::error_code&) override {}

    std::uint64_t room;
};
}

TEST_CASE("Every frame added is counted as in the file or as lost, when the file fails with frames waiting")
{
    std::vector<float> samples(1000, 0.5f);
    const float* channels[] = {samples.data()};

    BackgroundAudioWriter writer(std::make_unique<FullAfter>(2500), 100000, 1h);
    std::uint64_t added = 0;
    for (int i = 0; i < 20; ++i)
        if (writer.write(channels, samples.size()))
            added += samples.size();

    std::error_code error;
    writer.finish(error);
    CHECK(error);
    CHECK(writer.failed());
    CHECK(added == 20000);
    CHECK(writer.framesWritten() + writer.framesLost() == added);
    CHECK(writer.framesWritten() <= 2500);
    CHECK(writer.framesLost() >= 17500);
}

TEST_CASE("A function given what is written that throws fails the writer and doesn't end the program")
{
    ScratchFolder folder;
    std::vector<float> samples(1000, 0.5f);
    const float* channels[] = {samples.data()};
    BackgroundAudioWriter writer(
        files::createAudio(folder.path / "take.wav", {"WAV", 48000, 1, 16, false, 0, {}}), 10000, 1h,
        [](const float* const*, std::size_t) { throw std::runtime_error("the program's own"); });
    REQUIRE(writer.write(channels, samples.size()));
    CHECK(soon([&] { return writer.failed(); }));
    std::error_code error;
    writer.finish(error);
    CHECK(error);
}

TEST_CASE("A writer that was never made leaves a background writer failed from the start, not a crash")
{
    ScratchFolder folder;
    std::error_code refused;
    auto none = files::createAudio(folder.path / "take.wav", {"WAV", 48000, 1, 12, false, 0, {}}, refused);
    REQUIRE(none == nullptr);

    BackgroundAudioWriter writer(std::move(none), 1000);
    CHECK(writer.failed());
    std::vector<float> samples(10, 0.5f);
    const float* channels[] = {samples.data()};
    CHECK_FALSE(writer.write(channels, samples.size()));
    std::error_code error;
    writer.finish(error);
    CHECK(error);
    CHECK_THROWS_AS(BackgroundAudioWriter(nullptr, 1000).finish(), std::filesystem::filesystem_error);
}
