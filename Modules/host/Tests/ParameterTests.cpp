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

namespace
{

using Told = std::vector<std::pair<PluginEdit, bool>>;

FakePluginFormat::Script scripted()
{
    FakePluginFormat::Script script;
    script.parameters = {{1, "Gain", "", 1.0, 0, true, false, false, false, false},
                         {2, "Meter", "dB", 0.0, 0, false, true, false, false, false},
                         {3, "Mode", "", 0.0, 3, true, false, false, false, false}};
    script.gainParameter = 1;
    return script;
}

// What the plugin's own edits told the program, each checked as told on the main thread.
void listen(Rig& rig, Plugin& plugin, Told& told)
{
    plugin.onEdit = [&rig, &told](const PluginEdit& edit, bool byUser) {
        told.emplace_back(edit, byUser);

        if (!rig.main.isCurrent()())
            rig.wrong.push_back("an edit was told off the main thread");
    };
}

std::size_t count(const std::vector<std::string>& calls, const std::string& call)
{
    return static_cast<std::size_t>(std::count(calls.begin(), calls.end(), call));
}

std::vector<float> played(Plugin& plugin, int frames, std::vector<PluginParameterChange> changes)
{
    std::vector<float> left(static_cast<std::size_t>(frames), 1.0f), right(left);
    float* channels[] = {left.data(), right.data()};
    plugin.process(channels, 2, frames, changes.data(), static_cast<int>(changes.size()));
    return left;
}

constexpr auto begin = PluginEdit::Kind::begin, value = PluginEdit::Kind::value, end = PluginEdit::Kind::end,
               reported = PluginEdit::Kind::reported;

}

TEST_CASE("A plugin's own edit reaches its processing and the program with its gesture, and isn't shown back to it")
{
    Rig rig;
    auto plugin = rig.made();
    Told told;
    listen(rig, *plugin, told);
    REQUIRE(plugin->prepare({48000.0, 64, false}));
    const auto before = rig.fake.calls("plugin").size();

    rig.fake.edit("plugin", {begin, 1, 0.0});
    rig.fake.edit("plugin", {value, 1, 0.25});
    rig.fake.edit("plugin", {end, 1, 0.0});
    REQUIRE(rig.main.turnUntil([&] { return told.size() == 3; }));

    CHECK(told == Told{{{begin, 1, 0.0}, true}, {{value, 1, 0.25}, true}, {{end, 1, 0.0}, true}});
    CHECK(plugin->parameter(1) == 0.25);
    CHECK(through(*plugin) == 0.25f);
    CHECK(count(rig.callsSince(before), "showParameterValue") == 0);
    rig.checkNoMisuse();
}

TEST_CASE("A plugin whose own edit leaves the value it shows as it was is shown the value")
{
    Rig rig;
    auto script = scripted();
    script.forgetsOwnEdits = true;
    rig.fake.script("plugin", script);
    auto plugin = rig.made();
    Told told;
    listen(rig, *plugin, told);
    const auto before = rig.fake.calls("plugin").size();

    rig.fake.edit("plugin", {value, 3, 0.75});
    REQUIRE(rig.main.turnUntil([&] { return told.size() == 1; }));
    rig.main.settle();

    CHECK(count(rig.callsSince(before), "showParameterValue") == 1);
    CHECK(plugin->parameter(3) == 0.75);
    rig.checkNoMisuse();
}

TEST_CASE("A plugin's edit of a parameter it never listed is passed over")
{
    Rig rig;
    auto plugin = rig.made();
    Told told;
    listen(rig, *plugin, told);

    rig.fake.edit("plugin", {begin, 99, 0.0});
    rig.fake.edit("plugin", {value, 99, 0.5});
    rig.fake.edit("plugin", {value, 3, 0.5});
    REQUIRE(rig.main.turnUntil([&] { return !told.empty(); }));
    rig.main.settle();

    CHECK(told == Told{{{value, 3, 0.5}, true}});
    CHECK(plugin->parameter(99) == 0.0);
    rig.checkNoMisuse();
}

TEST_CASE("A value a plugin edits back while it is shown it isn't answered or taken as the user's, rounded or not")
{
    for (const int echoes : {1, 2})
    {
        INFO(echoes);
        Rig rig;
        auto script = scripted();
        script.echoes = echoes;
        rig.fake.script("plugin", script);
        auto plugin = rig.made();
        Told told;
        listen(rig, *plugin, told);
        const auto before = rig.fake.calls("plugin").size();

        REQUIRE(plugin->setParameter(3, 0.3));

        for (int i = 0; i < 20; ++i)
            rig.main.settle();

        CHECK(count(rig.callsSince(before), "showParameterValue") == 1);
        CHECK(told.empty());
        CHECK(plugin->parameter(3) == 0.3);
        rig.checkNoMisuse();
    }
}

TEST_CASE("A plugin's edits from another thread, and from inside its processing, are told on the main thread")
{
    Rig rig;
    std::atomic<bool> sent{false};
    auto script = scripted();
    script.inside = [&rig, &sent](std::string_view call) {
        if (call == "process" && !sent.exchange(true))
            rig.fake.edit("plugin", {value, 3, 0.25});
    };
    rig.fake.script("plugin", script);
    auto plugin = rig.made();
    Told told;
    listen(rig, *plugin, told);
    REQUIRE(plugin->prepare({48000.0, 64, false}));

    std::thread([&rig] { rig.fake.edit("plugin", {value, 1, 0.5}); }).join();
    std::thread([&plugin] { through(*plugin); }).join();
    REQUIRE(rig.main.turnUntil([&] { return told.size() == 2; }));

    CHECK(std::count(told.begin(), told.end(), std::make_pair(PluginEdit{value, 1, 0.5}, true)) == 1);
    CHECK(std::count(told.begin(), told.end(), std::make_pair(PluginEdit{value, 3, 0.25}, true)) == 1);
    rig.checkNoMisuse();
}

TEST_CASE("Values a plugin's processing reports reach the program and are shown to it, never as the user's")
{
    Rig rig;
    auto script = scripted();
    script.reports = {{2, 0, 0.75}, {99, 0, 0.5}};
    rig.fake.script("plugin", script);
    auto plugin = rig.made();
    Told told;
    listen(rig, *plugin, told);
    REQUIRE(plugin->prepare({48000.0, 64, false}));
    const auto before = rig.fake.calls("plugin").size();

    for (int i = 0; i < 5; ++i)
        through(*plugin);

    REQUIRE(rig.main.turnUntil([&] { return !told.empty(); }, 2s));
    rig.main.settle();

    CHECK(told.front() == std::make_pair(PluginEdit{reported, 2, 0.75}, false));
    CHECK(std::all_of(told.begin(), told.end(), [](const auto& one) { return one.first.id == 2 && !one.second; }));
    CHECK(plugin->parameter(2) == 0.75);
    CHECK(count(rig.callsSince(before), "showParameterValue") >= 1);
    rig.checkNoMisuse();
}

TEST_CASE("Edits within 250 ms of a plugin's state being given aren't the user's, nor is its state changing then")
{
    Rig rig;
    auto plugin = rig.made();
    Told told;
    listen(rig, *plugin, told);
    std::vector<PluginChange> changes;
    plugin->onChange = [&changes](PluginChange change) { changes.push_back(change); };

    REQUIRE(plugin->loadState("fake state", PluginStateUse::document));
    rig.fake.edit("plugin", {value, 3, 0.25});
    rig.fake.notify("plugin", PluginNotice::state);
    REQUIRE(rig.main.turnUntil([&] { return told.size() == 1; }));
    rig.main.settle();

    CHECK(told.back().second == false);
    CHECK(std::count(changes.begin(), changes.end(), PluginChange::state) == 0);

    REQUIRE(rig.main.turnUntil([] { return false; }, 300ms) == false);
    rig.fake.edit("plugin", {value, 3, 0.5});
    rig.fake.notify("plugin", PluginNotice::state);
    REQUIRE(rig.main.turnUntil([&] { return told.size() == 2; }));
    rig.main.settle();

    CHECK(told.back() == std::make_pair(PluginEdit{value, 3, 0.5}, true));
    CHECK(std::count(changes.begin(), changes.end(), PluginChange::state) == 1);
    rig.checkNoMisuse();
}

TEST_CASE("A plugin saying its state changed many times at once is told once")
{
    Rig rig;
    auto plugin = rig.made();
    std::vector<PluginChange> changes;
    plugin->onChange = [&changes](PluginChange change) { changes.push_back(change); };

    for (int i = 0; i < 100; ++i)
        rig.fake.notify("plugin", PluginNotice::state);

    rig.main.settle();
    CHECK(std::count(changes.begin(), changes.end(), PluginChange::state) == 1);
}

TEST_CASE("The program's automation reaches a plugin at its frames, in a block split for it, and is shown to it")
{
    Rig rig;
    auto plugin = rig.made();
    Told told;
    listen(rig, *plugin, told);
    REQUIRE(plugin->prepare({48000.0, 32, false}));
    const auto before = rig.fake.calls("plugin").size();

    const auto out = played(*plugin, 64, {{1, 0, 0.5}, {99, 8, 0.5}, {1, 16, 0.25}, {1, 40, 0.75}});

    CHECK(out[0] == 0.5f);
    CHECK(out[15] == 0.5f);
    CHECK(out[16] == 0.25f);
    CHECK(out[39] == 0.25f);
    CHECK(out[40] == 0.75f);
    CHECK(out[63] == 0.75f);
    REQUIRE(rig.main.turnUntil([&] { return plugin->parameter(1) == 0.75; }, 2s));
    rig.main.settle();
    CHECK(count(rig.callsSince(before), "showParameterValue") >= 1);
    CHECK(told.empty());
    rig.checkNoMisuse();
}
