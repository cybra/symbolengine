#include <symbolengine/host/FakePluginFormat.h>
#include <symbolengine/host/Plugin.h>

#include "TestMainThread.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

using namespace symbolengine;
using namespace std::chrono_literals;

namespace
{

struct Rig
{
    TestMainThread main;
    FakePluginFormat fake{main.isCurrent(), main.post()};
    std::vector<std::string> wrong;
    PluginHost host{
        {main.post(), main.isCurrent(), 2000ms, [this](const std::string& what) { wrong.push_back(what); }}};

    // The time each block carried, a position of -1 for none.
    std::mutex lock;
    std::vector<PluginTime> times;

    std::shared_ptr<Plugin> made()
    {
        FakePluginFormat::Script script;
        script.onBlock = [this](const PluginBlock& block) {
            const std::lock_guard held(lock);
            times.push_back(block.time != nullptr ? *block.time : PluginTime{false, false, false, -1, 0, 0.0, 0, 0});
        };
        fake.script("timed", script);
        auto plugin = host.make(fake, FakePluginFormat::describe("timed"));
        REQUIRE(main.turnUntil([&] { return plugin->state() != Plugin::State::making; }));
        return plugin;
    }

    void checkNoMisuse()
    {
        const auto misuses = fake.misuses();
        INFO((misuses.empty() ? std::string() : misuses.front()));
        CHECK(misuses.empty());
        CHECK(wrong.empty());
    }
};

void play(Plugin& plugin, int frames, const PluginTime* time)
{
    std::vector<float> left(static_cast<std::size_t>(frames), 1.0f), right(left);
    float* channels[] = {left.data(), right.data()};
    plugin.process(channels, 2, frames, nullptr, 0, time);
}

}

TEST_CASE("Each block carries the program's time, a split block's at its own first frame")
{
    Rig rig;
    auto plugin = rig.made();
    REQUIRE(plugin->prepare({48000.0, 32, false}));
    const PluginTime time{true, false, false, 1000, 5'000'000'000, 90.0, 3, 4};

    play(*plugin, 64, &time);

    REQUIRE(rig.times.size() == 2);
    CHECK(rig.times[0].playing);
    CHECK(rig.times[0].position == 1000);
    CHECK(rig.times[1].position == 1032);
    CHECK(rig.times[0].systemNanoseconds == 5'000'000'000);
    CHECK(std::abs(rig.times[1].systemNanoseconds - (5'000'000'000 + 666'667)) <= 1);
    CHECK(rig.times[1].tempo == 90.0);
    CHECK(rig.times[1].numerator == 3);
    CHECK(rig.times[1].denominator == 4);
    rig.checkNoMisuse();
}

TEST_CASE("Without the program's time, each block carries a position counting the frames played, not playing, and "
          "the steady clock's time")
{
    Rig rig;
    auto plugin = rig.made();
    REQUIRE(plugin->prepare({48000.0, 64, false}));

    for (int i = 0; i < 3; ++i)
        play(*plugin, 64, nullptr);

    REQUIRE(rig.times.size() == 3);
    CHECK(rig.times[0].position == 0);
    CHECK(rig.times[1].position == 64);
    CHECK(rig.times[2].position == 128);
    CHECK_FALSE(rig.times[2].playing);
    CHECK(rig.times[0].systemNanoseconds > 0);
    CHECK(rig.times[1].systemNanoseconds > rig.times[0].systemNanoseconds);
    rig.checkNoMisuse();
}

TEST_CASE("A discontinuity in the program's time stops and starts a plugin's processing before the block")
{
    Rig rig;
    auto plugin = rig.made();
    REQUIRE(plugin->prepare({48000.0, 64, false}));
    PluginTime time{true, false, false, 0, 0, 0.0, 0, 0};
    play(*plugin, 64, &time);
    const auto before = rig.fake.calls("timed").size();

    time.position = 480000;
    time.discontinuity = true;
    play(*plugin, 64, &time);

    const auto calls = rig.fake.calls("timed");
    CHECK(std::vector<std::string>(calls.begin() + static_cast<std::ptrdiff_t>(before), calls.end()) ==
          std::vector<std::string>{"stopProcessing", "startProcessing"});
    REQUIRE(rig.times.size() == 2);
    CHECK(rig.times[1].position == 480000);
    rig.checkNoMisuse();
}
