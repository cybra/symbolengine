#include <symbolengine/host/FakePluginFormat.h>
#include <symbolengine/host/Plugin.h>

#include "TestMainThread.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <string>
#include <thread>
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

    Rig()
    {
        FakePluginFormat::Script script;
        script.parameters = {{1, "Gain", "", 1.0, 0, true, false, false, false, false},
                             {2, "Meter", "dB", 0.0, 0, false, true, false, false, false},
                             {3, "Mode", "", 0.0, 3, true, false, false, false, false}};
        script.gainParameter = 1;
        fake.script("plugin", script);
    }

    std::shared_ptr<Plugin> made()
    {
        auto plugin = host.make(fake, FakePluginFormat::describe("plugin"));
        REQUIRE(main.turnUntil([&] { return plugin->state() != Plugin::State::making; }));
        return plugin;
    }

    std::vector<std::string> callsSince(std::size_t from)
    {
        auto all = fake.calls("plugin");
        return {all.begin() + static_cast<std::ptrdiff_t>(std::min(from, all.size())), all.end()};
    }

    void checkNoMisuse()
    {
        const auto misuses = fake.misuses();
        INFO((misuses.empty() ? std::string() : misuses.front()));
        CHECK(misuses.empty());
        CHECK(wrong.empty());
    }
};

float through(Plugin& plugin)
{
    std::vector<float> left(64, 1.0f), right(64, 1.0f);
    float* channels[] = {left.data(), right.data()};
    plugin.process(channels, 2, 64);
    return left.back();
}

bool has(const std::vector<std::string>& calls, const std::string& call)
{
    return std::find(calls.begin(), calls.end(), call) != calls.end();
}

}

TEST_CASE("A plugin's parameters and their values are read once it is made")
{
    Rig rig;
    auto plugin = rig.made();
    const auto parameters = plugin->parameters();

    REQUIRE(parameters.size() == 3);
    CHECK(parameters[0].name == "Gain");
    CHECK(parameters[1].readOnly);
    CHECK(parameters[2].steps == 3);
    CHECK(plugin->parameter(1) == 1.0);
    CHECK(plugin->parameterText(3, 0.5) == "3:0.500000");
    rig.checkNoMisuse();
}

TEST_CASE("A value set while nothing plays a plugin reaches its processing before its state is taken")
{
    Rig rig;
    auto plugin = rig.made();
    REQUIRE(plugin->prepare({48000.0, 64, false}));
    const auto before = rig.fake.calls("plugin").size();

    REQUIRE(plugin->setParameter(1, 0.25));
    CHECK(plugin->parameter(1) == 0.25);
    CHECK_FALSE(has(rig.callsSince(before), "flush"));

    const auto state = plugin->saveState(PluginStateUse::document);
    REQUIRE(state.has_value());
    CHECK(state->find("1=0.250000") != std::string::npos);
    const auto calls = rig.callsSince(before);
    CHECK(std::find(calls.begin(), calls.end(), "flush") < std::find(calls.begin(), calls.end(), "state"));

    // And plays at it, once the device plays it.
    CHECK(through(*plugin) == 0.25f);
    rig.checkNoMisuse();
}

TEST_CASE("A value set before a plugin is prepared reaches its processing once it is")
{
    Rig rig;
    auto plugin = rig.made();
    REQUIRE(plugin->setParameter(1, 0.5));
    REQUIRE(plugin->prepare({48000.0, 64, false}));

    CHECK(has(rig.fake.calls("plugin"), "flush"));
    CHECK(through(*plugin) == 0.5f);
    rig.checkNoMisuse();
}

TEST_CASE("A value set while a device plays a plugin reaches it with a block, and its state taken after has it")
{
    Rig rig;
    auto plugin = rig.made();
    REQUIRE(plugin->prepare({48000.0, 64, false}));
    std::atomic<bool> running{true};
    std::atomic<float> heard{0.0f};
    std::thread audio([&] {
        while (running)
        {
            heard = through(*plugin);
            std::this_thread::sleep_for(200us);
        }
    });

    REQUIRE(rig.main.turnUntil([&] { return heard == 1.0f; }, 2s));
    const auto before = rig.fake.calls("plugin").size();
    REQUIRE(plugin->setParameter(1, 0.125));
    const auto state = plugin->saveState(PluginStateUse::document);
    const bool playedIt = rig.main.turnUntil([&] { return heard == 0.125f; }, 2s);
    running = false;
    audio.join();

    CHECK(playedIt);
    REQUIRE(state.has_value());
    CHECK(state->find("1=0.125000") != std::string::npos);
    CHECK_FALSE(has(rig.callsSince(before), "flush"));
    rig.checkNoMisuse();
}

TEST_CASE("A parameter that can't be written, or one a plugin doesn't have, isn't set")
{
    Rig rig;
    auto plugin = rig.made();

    CHECK_FALSE(plugin->setParameter(2, 0.5));
    CHECK_FALSE(plugin->setParameter(99, 0.5));
    CHECK(plugin->parameter(2) == 0.0);
    rig.checkNoMisuse();
}

TEST_CASE("Values set many times before a block reach the plugin as their last")
{
    Rig rig;
    auto plugin = rig.made();
    REQUIRE(plugin->prepare({48000.0, 64, false}));

    for (int i = 1; i <= 100; ++i)
        REQUIRE(plugin->setParameter(1, i / 100.0));

    const auto state = plugin->saveState(PluginStateUse::document);
    REQUIRE(state.has_value());
    CHECK(state->find("1=1.000000") != std::string::npos);
    CHECK(through(*plugin) == 1.0f);
    rig.checkNoMisuse();
}
