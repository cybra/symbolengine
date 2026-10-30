#include <symbolengine/host/FakePluginFormat.h>
#include <symbolengine/host/Plugin.h>

#include "TestMainThread.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace symbolengine;
using namespace std::chrono_literals;

namespace
{

std::int64_t nanosecondsNow()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Spends the time given, busy as a plugin working hard is.
void spendBusy(std::chrono::microseconds time)
{
    const auto until = std::chrono::steady_clock::now() + time;
    while (std::chrono::steady_clock::now() < until)
    {
    }
}

// A fake plugin that spends as long as it is told in each block and throws from its processing once told to, and others
// in the same host.
struct Rig
{
    TestMainThread main;
    FakePluginFormat fake{main.isCurrent(), main.post()};
    PluginHost host{{main.post(), main.isCurrent(), 2000ms, nullptr}};
    std::atomic<std::int64_t> spend{0};
    std::atomic<bool> fail{false};
    std::shared_ptr<Plugin> plugin;

    Rig()
    {
        FakePluginFormat::Script script;
        script.onBlock = [this](const PluginBlock&) {
            if (fail)
                throw std::runtime_error("told to fail");

            spendBusy(std::chrono::microseconds(spend.load()));
        };
        fake.script("busy", script);
        plugin = host.make(fake, FakePluginFormat::describe("busy"));
        REQUIRE(main.turnUntil([&] { return plugin->state() != Plugin::State::making; }));
    }

    // Another, prepared for blocks of 10 ms, that spends as many microseconds in each as the value given says.
    std::shared_ptr<Plugin> another(const std::string& id, std::atomic<std::int64_t>& spends)
    {
        FakePluginFormat::Script script;
        script.onBlock = [&spends](const PluginBlock&) { spendBusy(std::chrono::microseconds(spends.load())); };
        fake.script(id, script);
        auto made = host.make(fake, FakePluginFormat::describe(id));
        REQUIRE(main.turnUntil([&] { return made->state() != Plugin::State::making; }));
        REQUIRE(made->prepare({48000.0, 480, false}));
        return made;
    }

    void play(int blocks, std::chrono::microseconds each, int frames = 480)
    {
        spend = each.count();
        std::vector<float> left(static_cast<std::size_t>(frames)), right = left;
        float* channels[] = {left.data(), right.data()};

        for (int i = 0; i < blocks; ++i)
            plugin->process(channels, 2, frames);
    }
};

}

TEST_CASE("A plugin's time in each block is measured against the time the block lasts, its worst block kept until its "
          "timing is taken",
          "[timing]")
{
    Rig rig;
    REQUIRE(rig.plugin->prepare({48000.0, 480, false}));

    auto timing = rig.plugin->timing();
    CHECK(timing.blocks == 0);
    CHECK(timing.lateBlocks == 0);
    CHECK(timing.load == 0.0);
    CHECK(timing.worstShare == 0.0);

    // Blocks of 10 ms, in each of which it spends 2 ms, but for one of 6 ms, played one after another, as fast as it
    // takes them: what counts is its own time against the block's.
    rig.play(10, 2ms);
    const auto before = nanosecondsNow();
    rig.play(1, 6ms);
    const auto after = nanosecondsNow();
    rig.play(10, 2ms);

    timing = rig.plugin->timing();
    INFO("load " << timing.load << ", worst " << timing.worstShare << " taking " << timing.worstNanoseconds << " ns");
    CHECK(timing.blocks == 21);
    CHECK(timing.lateBlocks == 0);
    CHECK(timing.load >= 46.0 / 210.0);
    CHECK(timing.load < 0.5);
    CHECK(timing.worstShare >= 0.6);
    CHECK(timing.worstShare < 0.95);
    CHECK(timing.worstNanoseconds >= 6'000'000);
    CHECK(timing.worstAt >= before);
    CHECK(timing.worstAt + 6'000'000 <= after);

    // Taken, its load and worst block start again from then; what it has played since it was made goes on.
    const auto taken = rig.plugin->takeTiming();
    CHECK(taken.worstShare == timing.worstShare);
    CHECK(taken.worstAt == timing.worstAt);
    timing = rig.plugin->timing();
    CHECK(timing.blocks == 21);
    CHECK(timing.load == 0.0);
    CHECK(timing.worstShare == 0.0);
    CHECK(timing.worstNanoseconds == 0);

    // A block it alone takes longer over than the block lasts is late, its share over the whole; and a shorter block's
    // time is its own frames', so 1 ms of 5 ms is a fifth.
    rig.play(1, 12ms);
    rig.play(5, 1ms, 240);
    timing = rig.plugin->timing();
    INFO("load " << timing.load);
    CHECK(timing.blocks == 27);
    CHECK(timing.lateBlocks == 1);
    CHECK(timing.load >= 17.0 / 35.0);
    CHECK(timing.load < 0.8);
    CHECK(timing.worstShare >= 1.2);
    CHECK(timing.worstNanoseconds >= 12'000'000);
}

TEST_CASE("Blocks that pass a plugin by, before it is prepared and once it has failed, aren't timed")
{
    Rig rig;
    rig.play(5, 0us);
    CHECK(rig.plugin->timing().blocks == 0);

    REQUIRE(rig.plugin->prepare({48000.0, 480, false}));
    rig.play(3, 0us);
    rig.fail = true;
    rig.play(1, 0us);
    rig.fail = false;
    REQUIRE(rig.main.turnUntil([&] { return rig.plugin->state() == Plugin::State::failed; }));
    rig.play(5, 0us);

    // The block it failed in is counted, its processing having run.
    CHECK(rig.plugin->timing().blocks == 4);
}

TEST_CASE("The plugin that alone took longer than its block lasts is named for a time its block spans, or else the one "
          "that took the most of it, a quarter of its block or more, and none where none did",
          "[timing]")
{
    Rig rig;
    std::atomic<std::int64_t> alpha{0}, beta{0}, gamma{0};
    auto first = rig.another("Alpha", alpha);
    const auto second = rig.another("Beta", beta);
    const auto third = rig.another("Gamma", gamma);
    std::vector<float> left(480), right(480);
    float* channels[] = {left.data(), right.data()};

    // A device's block playing the three in turn, as a chain does, each spending as many microseconds as given and
    // Beta given as many frames; the time it spanned.
    const auto cycle = [&](std::int64_t a, std::int64_t b, std::int64_t c, int betaFrames = 480) {
        alpha = a;
        beta = b;
        gamma = c;
        const auto began = nanosecondsNow();
        first->process(channels, 2, 480);
        second->process(channels, 2, betaFrames);
        third->process(channels, 2, 480);
        return std::pair{began, nanosecondsNow()};
    };

    CHECK_FALSE(rig.host.slowestBetween(0, nanosecondsNow()).has_value());

    // Alpha alone takes longer than its 10 ms block.
    const auto [began, ended] = cycle(12'000, 0, 0);
    auto late = rig.host.slowestBetween(began, ended);
    REQUIRE(late.has_value());
    CHECK(late->plugin.name == "Alpha");
    CHECK(late->alone);
    CHECK(late->nanoseconds >= 12'000'000);
    CHECK(late->blockNanoseconds == 10'000'000);
    CHECK(late->at >= began);
    CHECK(late->at < ended);

    // None alone, the three together late, Gamma taking the most.
    const auto together = cycle(1000, 4000, 7000);
    late = rig.host.slowestBetween(together.first, together.second);
    REQUIRE(late.has_value());
    CHECK(late->plugin.name == "Gamma");
    CHECK_FALSE(late->alone);
    CHECK(late->nanoseconds >= 7'000'000);

    // None taking a quarter of its block.
    const auto spread = cycle(1000, 1000, 1000);
    CHECK_FALSE(rig.host.slowestBetween(spread.first, spread.second).has_value());

    // Beta alone late in its block of 5 ms is named before Gamma, which took longer but not its whole block.
    const auto shorter = cycle(0, 6000, 8000, 240);
    late = rig.host.slowestBetween(shorter.first, shorter.second);
    REQUIRE(late.has_value());
    CHECK(late->plugin.name == "Beta");
    CHECK(late->alone);
    CHECK(late->blockNanoseconds == 5'000'000);

    // What was found before is still found, by its name once the plugin is gone, and a time with nothing in it names
    // none.
    first.reset();
    rig.main.settle();
    late = rig.host.slowestBetween(began, ended);
    REQUIRE(late.has_value());
    CHECK(late->plugin.name == "Alpha");
    CHECK_FALSE(rig.host.slowestBetween(ended + 1, together.first - 1).has_value());
}
