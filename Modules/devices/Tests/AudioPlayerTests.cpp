// The devices section's lessons for playing files: the player against generated sources, the test's thread playing
// the audio thread's part.

#include "Helpers.h"

#include <symbolengine/devices/AudioPlayer.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <numbers>
#include <optional>
#include <random>
#include <stdexcept>

using namespace symbolengine;
using Kind = AudioPlayer::Event::Kind;

namespace
{
// A source of the given length whose samples are their own frame numbers, scaled, negative on the second channel.
class Counting final : public AudioPlayerSource
{
public:
    explicit Counting(std::int64_t frames, double rate = 48000.0, int channels = 2)
        : frames_(frames), rate_(rate), channels_(channels)
    {
    }

    static float at(std::int64_t frame, int channel)
    {
        const auto value = static_cast<float>(frame + 1) / 16777216.0f;
        return channel == 0 ? value : -value;
    }

    int channels() const override { return channels_; }
    double sampleRate() const override { return rate_; }
    std::int64_t length() const override { return frames_; }

    void read(float* const* into, std::int64_t from, std::size_t frames) override
    {
        if (holdAt >= 0 && from <= holdAt && holdAt < from + static_cast<std::int64_t>(frames) && !held.exchange(true))
            std::this_thread::sleep_for(holdFor);
        for (int c = 0; c < channels_; ++c)
            for (std::size_t i = 0; i < frames; ++i)
                into[c][i] = at(from + static_cast<std::int64_t>(i), c);
    }

    // A read that reaches this frame is held up once, as a slow disk would.
    std::int64_t holdAt = -1;
    std::chrono::milliseconds holdFor{0};
    std::atomic<bool> held{false};

private:
    std::int64_t frames_;
    double rate_;
    int channels_;
};

// Counts as Counting does until a frame, past which it can't be read, as a file on a drive just unplugged.
class FailsAt final : public AudioPlayerSource
{
public:
    FailsAt(std::int64_t frames, std::int64_t failing, double rate) : frames_(frames), failing_(failing), rate_(rate) {}

    int channels() const override { return 2; }
    double sampleRate() const override { return rate_; }
    std::int64_t length() const override { return frames_; }

    void read(float* const* into, std::int64_t at, std::size_t frames) override
    {
        if (at + static_cast<std::int64_t>(frames) > failing_)
            throw std::runtime_error("the drive has gone");

        for (int c = 0; c < 2; ++c)
            for (std::size_t i = 0; i < frames; ++i)
                into[c][i] = Counting::at(at + static_cast<std::int64_t>(i), c);
    }

private:
    std::int64_t frames_, failing_;
    double rate_;
};

class Tone final : public AudioPlayerSource
{
public:
    Tone(double frequency, double rate, std::int64_t frames) : frequency_(frequency), rate_(rate), frames_(frames) {}

    int channels() const override { return 1; }
    double sampleRate() const override { return rate_; }
    std::int64_t length() const override { return frames_; }

    void read(float* const* into, std::int64_t from, std::size_t frames) override
    {
        for (std::size_t i = 0; i < frames; ++i)
            into[0][i] = static_cast<float>(
                0.5 * std::sin(2.0 * std::numbers::pi * frequency_ * static_cast<double>(from + static_cast<std::int64_t>(i)) / rate_));
    }

private:
    double frequency_, rate_;
    std::int64_t frames_;
};

// The player and the buffers the test plays it into, the device at 48 kHz.
struct Deck
{
    explicit Deck(AudioPlayer::Options options = quiet(), int channels = 2)
        : player(std::move(options)), left(4096), right(4096)
    {
        player.prepare(48000.0, channels);
    }

    static AudioPlayer::Options quiet()
    {
        AudioPlayer::Options options;
        options.fade = 0ms;
        return options;
    }

    // Plays frames in blocks of `block`, keeping everything played, each block once the reader has it ready, as a
    // device's pace allows, unless asked to play at once whatever is ready. A reader slowed by the sanitizers on a
    // crowded runner is given up to a second a block; near a source's end, where fewer than a block remain, the wait
    // runs out and plays what there is.
    std::vector<std::vector<float>> play(std::int64_t frames, int block = 512, bool waiting = true)
    {
        std::vector<std::vector<float>> out(2);
        for (std::int64_t done = 0; done < frames;)
        {
            const auto n = static_cast<int>(std::min<std::int64_t>(block, frames - done));
            if (waiting)
                soon([&] { return player.ready() >= static_cast<std::size_t>(n) || !player.playing(); }, 1000ms);
            float* channels[2]{left.data(), right.data()};
            player.render(channels, 2, n);
            out[0].insert(out[0].end(), left.begin(), left.begin() + n);
            out[1].insert(out[1].end(), right.begin(), right.begin() + n);
            done += n;
        }
        return out;
    }

    void waitReady(std::size_t frames = 4096) const
    {
        REQUIRE(soon([&] { return player.ready() >= frames; }));
    }

    AudioPlayer player;
    std::vector<float> left, right;
};
}

TEST_CASE("A source at the device's rate plays sample for sample from where it starts")
{
    Deck deck;
    deck.player.play(std::make_shared<Counting>(48000), 1000);
    deck.waitReady();
    const auto out = deck.play(20000, 333);
    for (std::size_t i = 0; i < out[0].size(); ++i)
    {
        REQUIRE(out[0][i] == Counting::at(1000 + static_cast<std::int64_t>(i), 0));
        REQUIRE(out[1][i] == Counting::at(1000 + static_cast<std::int64_t>(i), 1));
    }
    CHECK(deck.player.position() == 21000);
    CHECK(deck.player.playing());
}

TEST_CASE("Frames said to be ready play at once, whenever it is asked")
{
    // Asked as often as the test's thread can, the moment a play starts, for a frame to play the moment one is ready.
    for (int attempt = 0; attempt < 2000; ++attempt)
    {
        Deck deck;
        deck.player.play(std::make_shared<Counting>(48000), 0);
        const auto until = std::chrono::steady_clock::now() + 5s;
        while (deck.player.ready() == 0)
            if (std::chrono::steady_clock::now() > until)
                FAIL("nothing was ready");
        float* channels[2]{deck.left.data(), deck.right.data()};
        deck.player.render(channels, 2, 1);
        INFO("attempt " << attempt);
        REQUIRE(deck.left[0] == Counting::at(0, 0));
    }
}

TEST_CASE("A mono source plays on both outputs")
{
    Deck deck;
    deck.player.play(std::make_shared<Counting>(48000, 48000.0, 1), 0);
    deck.waitReady();
    const auto out = deck.play(4096);
    for (std::size_t i = 0; i < out[0].size(); ++i)
        REQUIRE((out[0][i] == Counting::at(static_cast<std::int64_t>(i), 0) && out[1][i] == out[0][i]));
}

TEST_CASE("Starting and stopping fade by the exact curve, whatever the block size, and a stop returns at once")
{
    // A source of ones, fading over 5 ms, 240 frames at 48 kHz, played in blocks of every size from 1 up.
    struct Ones final : AudioPlayerSource
    {
        int channels() const override { return 2; }
        double sampleRate() const override { return 48000.0; }
        std::int64_t length() const override { return 1'000'000; }
        void read(float* const* into, std::int64_t, std::size_t frames) override
        {
            for (int c = 0; c < 2; ++c)
                std::fill(into[c], into[c] + frames, 1.0f);
        }
    };
    for (const int block : {1, 7, 64, 240, 512, 4096})
    {
        AudioPlayer::Options options;
        options.fade = 5ms;
        Deck deck(options);
        deck.player.play(std::make_shared<Ones>(), 0);
        deck.waitReady();
        const auto in = deck.play(480, block);
        for (std::size_t i = 0; i < 480; ++i)
            REQUIRE(std::abs(in[0][i] - static_cast<float>(std::min(1.0, static_cast<double>(i + 1) / 240.0))) < 1e-6f);

        const auto before = std::chrono::steady_clock::now();
        deck.player.stop();
        CHECK(std::chrono::steady_clock::now() - before < 10ms);
        CHECK(deck.player.playing());
        const auto out = deck.play(480, block);
        for (std::size_t i = 0; i < 480; ++i)
            REQUIRE(std::abs(out[0][i] - static_cast<float>(std::max(0.0, 1.0 - static_cast<double>(i + 1) / 240.0))) < 1e-6f);
        CHECK_FALSE(deck.player.playing());
    }
}

TEST_CASE("The end comes exactly at the last frame, then silence, and is said")
{
    for (const std::int64_t length : {511, 512, 513, 1000})
    {
        Events unused;
        std::atomic<int> finished{0};
        AudioPlayer::Options options = Deck::quiet();
        options.onEvent = [&](const AudioPlayer::Event& event) {
            if (event.kind == Kind::finished)
                ++finished;
        };
        Deck deck(options);
        deck.player.play(std::make_shared<Counting>(length), 0);
        REQUIRE(soon([&] { return deck.player.ready() >= static_cast<std::size_t>(length); }));
        const auto out = deck.play(length + 700);
        for (std::int64_t i = 0; i < length + 700; ++i)
            REQUIRE(out[0][static_cast<std::size_t>(i)] == (i < length ? Counting::at(i, 0) : 0.0f));
        CHECK_FALSE(deck.player.playing());
        CHECK(soon([&] { return finished == 1; }));
    }
}

TEST_CASE("A source that can't be read plays silence from there, says so once, and the program goes on")
{
    constexpr std::int64_t length = 48000, failing = 12000;
    std::atomic<int> failed{0}, finished{0};
    std::atomic<std::int64_t> failedAt{-1};
    AudioPlayer::Options options = Deck::quiet();
    options.onEvent = [&](const AudioPlayer::Event& event) {
        if (event.kind == Kind::readFailed)
        {
            ++failed;
            failedAt = event.position;
        }
        if (event.kind == Kind::finished)
            ++finished;
    };
    Deck deck(options);

    SECTION("at the device's rate, sample for sample up to the piece that failed")
    {
        deck.player.play(std::make_shared<FailsAt>(length, failing, 48000.0), 0);
        REQUIRE(soon([&] { return failed > 0; }));
        const auto out = deck.play(length + 700);
        std::size_t heard = 0;

        while (heard < out[0].size() && out[0][heard] == Counting::at(static_cast<std::int64_t>(heard), 0))
            ++heard;

        CHECK(heard > 0);
        CHECK(static_cast<std::int64_t>(heard) <= failing);
        CHECK(failedAt.load() == static_cast<std::int64_t>(heard));
        CHECK(std::all_of(out[0].begin() + static_cast<std::ptrdiff_t>(heard), out[0].end(),
                          [](float sample) { return sample == 0.0f; }));
        CHECK(std::all_of(out[1].begin() + static_cast<std::ptrdiff_t>(heard), out[1].end(),
                          [](float sample) { return sample == 0.0f; }));
    }

    SECTION("at another rate, through the resampler")
    {
        deck.player.play(std::make_shared<FailsAt>(length, failing, 44100.0), 0);
        REQUIRE(soon([&] { return failed > 0; }));
        const auto out = deck.play(length + 8000);
        CHECK(std::abs(out[0].back()) == 0.0f);
    }

    // Told once, however many reads failed, played to its end, and the next source plays as ever.
    CHECK(soon([&] { return finished == 1; }));
    CHECK(failed == 1);
    deck.player.play(std::make_shared<Counting>(1000), 0);
    REQUIRE(soon([&] { return deck.player.ready() >= 512; }));
    const auto next = deck.play(512);
    CHECK(next[0][100] == Counting::at(100, 0));
}

TEST_CASE("A loop's join is exact to the sample, whatever its length against the block size")
{
    for (const std::int64_t span : {1, 7, 509, 511, 512, 513})
    {
        Deck deck;
        deck.player.play(std::make_shared<Counting>(10000), 100, 100 + span, 100);
        deck.waitReady(2048);
        const auto out = deck.play(3000, 256);
        for (std::size_t i = 0; i < out[0].size(); ++i)
            REQUIRE(out[0][i] == Counting::at(100 + static_cast<std::int64_t>(i) % span, 0));
    }
}

TEST_CASE("A reader held up longer than it reads ahead gives silence for what it lacks, keeps time and says where", "[timing]")
{
    // Each stretch the reader lacked, in order; a busy computer may add stretches of its own beside the hold's.
    std::mutex lock;
    std::vector<std::pair<std::int64_t, std::int64_t>> stretches;
    AudioPlayer::Options options = Deck::quiet();
    options.readAhead = 300ms;
    options.onEvent = [&](const AudioPlayer::Event& event) {
        if (event.kind == Kind::underrun)
        {
            const std::lock_guard hold(lock);
            stretches.emplace_back(event.position, event.frames);
        }
    };
    Deck deck(options);
    auto source = std::make_shared<Counting>(48000 * 10);
    source->holdAt = 48000;
    source->holdFor = 1000ms;
    deck.player.play(source, 0);
    deck.waitReady(2048);

    // Three seconds at the device's pace by the clock, however long the test's sleeps take.
    std::vector<float> heard;
    const auto began = std::chrono::steady_clock::now();
    while (heard.size() < 144000)
    {
        const auto due = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count() * 48000.0;
        while (static_cast<double>(heard.size()) + 480 <= due && heard.size() < 144000)
        {
            const auto out = deck.play(480, 480, false);
            heard.insert(heard.end(), out[0].begin(), out[0].end());
        }
        std::this_thread::sleep_for(2ms);
    }

    // The stretch the hold made starts where the reader stopped and lasts at least the hold's second less what was read
    // ahead, then as long as the reader takes to come back, which a busy computer stretches; that it came back is the
    // last frame heard being the source's.
    const auto held = [&]() -> std::optional<std::pair<std::int64_t, std::int64_t>> {
        const std::lock_guard hold(lock);
        for (const auto& stretch : stretches)
            if (std::abs(stretch.first - 48000) <= 4800)
                return stretch;
        return std::nullopt;
    };
    REQUIRE(soon([&] { return held().has_value(); }));
    const auto [lackedFrom, lackedFrames] = *held();
    INFO(lackedFrames << " frames lacking from " << lackedFrom << ", among " << stretches.size() << " stretches");
    CHECK(static_cast<double>(lackedFrames) > 48000 * 0.5);

    // Every frame heard is the source's at its own place in time, or silence.
    for (std::size_t i = 0; i < heard.size(); ++i)
        REQUIRE((heard[i] == 0.0f || heard[i] == Counting::at(static_cast<std::int64_t>(i), 0)));
    CHECK(heard.back() == Counting::at(static_cast<std::int64_t>(heard.size()) - 1, 0));
    CHECK(deck.player.position() == static_cast<std::int64_t>(heard.size()));
}

TEST_CASE("A jump while playing never steps further between samples than the fade allows")
{
    AudioPlayer::Options options;
    options.fade = 5ms;
    Deck deck(options);
    deck.player.play(std::make_shared<Tone>(1000.0, 48000.0, 48000 * 10), 0);
    deck.waitReady();
    auto heard = deck.play(4800)[0];
    for (int jump = 0; jump < 10; ++jump)
    {
        // The reader ahead of what plays, as it is at a device's pace, so the play jumped from has frames to fade over;
        // played faster than that by a reader the sanitizers slow, it had one block, and stopped short of silence.
        deck.waitReady(2048);

        // Half a cycle out each time, where the tone is the opposite of what it was.
        deck.player.seek(deck.player.position() + 24 + jump * 1000);
        const auto out = deck.play(2400, 128);
        heard.insert(heard.end(), out[0].begin(), out[0].end());
    }
    double largest = 0.0;
    for (std::size_t i = 4800; i < heard.size(); ++i)
        largest = std::max(largest, static_cast<double>(std::abs(heard[i] - heard[i - 1])));
    // The tone steps at most 0.066 a sample; the fade adds at most 0.5 over 240.
    CHECK(largest < 0.07);
}

TEST_CASE("A source at another rate plays at the device's, within -90 dB of the tone it holds")
{
    Deck deck;
    deck.player.play(std::make_shared<Tone>(1000.0, 44100.0, 44100 * 5), 0);
    deck.waitReady();
    const auto out = deck.play(48000);
    double error = 0.0;
    for (std::size_t j = 4800; j < 43200; ++j)
    {
        const auto ideal = 0.5 * std::sin(2.0 * std::numbers::pi * 1000.0 * static_cast<double>(j) / 48000.0);
        error += (out[0][j] - ideal) * (out[0][j] - ideal);
    }
    CHECK(10.0 * std::log10(error / 38400.0) < -90.0);
    CHECK(std::abs(deck.player.position() - 44100) <= 2);
}

TEST_CASE("Playing, jumping and stopping thousands of times while blocks are played changes nothing it shouldn't")
{
    Deck deck;
    std::atomic<bool> done{false};
    std::thread audio([&] {
        std::vector<float> left(512), right(512);
        while (!done.load())
        {
            float* channels[2]{left.data(), right.data()};
            deck.player.render(channels, 2, 512);
        }
    });
    std::minstd_rand random(4);
    for (int i = 0; i < 3000; ++i)
    {
        switch (random() % 4)
        {
            case 0: deck.player.play(std::make_shared<Counting>(1 + random() % 100000), random() % 1000); break;
            case 1: deck.player.seek(random() % 50000); break;
            case 2: deck.player.stop(); break;
            default: deck.player.play(std::make_shared<Tone>(440.0, 44100.0, 100000), 0, -1, 0); break;
        }
    }
    // An hour, which blocks played as fast as they can won't finish before it is looked at.
    deck.player.play(std::make_shared<Counting>(48000 * 3600), 0);
    const bool moved = soon([&] { return deck.player.position() > 1000; });
    INFO("ready " << deck.player.ready() << ", position " << deck.player.position() << ", playing "
                  << deck.player.playing());
    CHECK(moved);
    done = true;
    audio.join();
    CHECK(deck.player.playing());
}

namespace
{
// A source whose reads wait until they are let go, as a disk that has stopped answering.
class Stalls final : public AudioPlayerSource
{
public:
    explicit Stalls(double rate = 48000.0) : rate_(rate) {}

    int channels() const override { return 2; }
    double sampleRate() const override { return rate_; }
    std::int64_t length() const override { return 480000; }

    void read(float* const* into, std::int64_t, std::size_t frames) override
    {
        reading = true;
        while (stalled.load())
            std::this_thread::sleep_for(1ms);
        for (int c = 0; c < 2; ++c)
            std::fill_n(into[c], frames, 0.25f);
    }

    std::atomic<bool> stalled{true}, reading{false};

private:
    double rate_;
};
}

TEST_CASE("Preparing a player doesn't wait for a read that has stalled, which holds up the stream that asks")
{
    // At a rate of its own, so what the read comes back to has a resampler, which preparing lets go of.
    Deck deck;
    const auto source = std::make_shared<Stalls>(44100.0);
    deck.player.play(source, 0);
    const auto until = std::chrono::steady_clock::now() + 5s;
    while (!source->reading.load())
        REQUIRE(std::chrono::steady_clock::now() < until);

    const auto before = std::chrono::steady_clock::now();
    deck.player.prepare(96000.0, 2);
    CHECK(std::chrono::steady_clock::now() - before < 500ms);

    source->stalled = false;
    deck.waitReady();
    float* channels[2]{deck.left.data(), deck.right.data()};
    deck.player.render(channels, 2, 512);
    deck.player.stop();
}

TEST_CASE("A player stopped between one play and the next isn't left saying it plays")
{
    Deck deck;
    deck.player.play(std::make_shared<Counting>(480000), 0);
    deck.waitReady();
    float* channels[2]{deck.left.data(), deck.right.data()};
    deck.player.render(channels, 2, 512);
    CHECK(deck.player.playing());

    // The next play has nothing ready for the block that leaves the first, and then is stopped too.
    const auto next = std::make_shared<Stalls>();
    deck.player.play(next, 0);
    deck.player.render(channels, 2, 512);
    deck.player.stop();
    next->stalled = false;
    deck.player.render(channels, 2, 512);
    deck.player.render(channels, 2, 512);

    CHECK_FALSE(deck.player.playing());
}

TEST_CASE("A jump while the player holds less than the fade takes still fades, from the last sample it played")
{
    // Read as far as a point and no further, as a disk that has fallen behind, so it soon holds next to nothing.
    class Runs final : public AudioPlayerSource
    {
    public:
        int channels() const override { return 2; }
        double sampleRate() const override { return 48000.0; }
        std::int64_t length() const override { return 480000; }

        void read(float* const* into, std::int64_t from, std::size_t frames) override
        {
            while (from >= 2048 && stall.load())
                std::this_thread::sleep_for(1ms);
            for (int c = 0; c < 2; ++c)
                std::fill_n(into[c], frames, 0.5f);
        }

        std::atomic<bool> stall{true};
    };

    AudioPlayer::Options options;
    options.fade = 5ms;
    Deck deck(options);
    const auto source = std::make_shared<Runs>();
    deck.player.play(source, 0);
    deck.waitReady(1024);

    float* channels[2]{deck.left.data(), deck.right.data()};
    while (deck.player.ready() > 64)
        deck.player.render(channels, 2, 32);

    // Played up to full level, it now holds a few frames of a fade that takes 240.
    deck.player.seek(0);
    deck.player.render(channels, 2, 512);
    float last = 0.5f;
    double largest = 0.0;
    for (int i = 0; i < 512; ++i)
    {
        largest = std::max(largest, static_cast<double>(std::abs(deck.left[static_cast<std::size_t>(i)] - last)));
        last = deck.left[static_cast<std::size_t>(i)];
    }
    CHECK(largest < 0.01);
    source->stall = false;
    deck.player.stop();
}
