#include <symbolengine/host/FakePluginFormat.h>
#include <symbolengine/host/Plugin.h>

#include "TestMainThread.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <stdexcept>
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

// A fake plugin that spends as long as it is told in each block, busy as a plugin working hard is, and throws from its
// processing once told to.
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

            const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(spend.load());
            while (std::chrono::steady_clock::now() < until)
            {
            }
        };
        fake.script("busy", script);
        plugin = host.make(fake, FakePluginFormat::describe("busy"));
        REQUIRE(main.turnUntil([&] { return plugin->state() != Plugin::State::making; }));
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
