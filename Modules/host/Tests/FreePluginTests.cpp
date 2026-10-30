#include "Scratch.h"
#include "TestMainThread.h"

#include <symbolengine/core/Paths.h>
#include <symbolengine/core/System.h>
#include <symbolengine/host/Plugin.h>
#include <symbolengine/host/PluginScanner.h>
#include <symbolengine/host/Vst3Format.h>

#if defined(__APPLE__)
    #include <symbolengine/host/AudioUnitFormat.h>
#endif

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace symbolengine;
using namespace std::chrono_literals;

// Plugins by other makers, which continuous integration downloads, each pinned by its checksum, and names the folder of
// in SYMBOLENGINE_FREE_PLUGINS. Anywhere else these tests skip, as they would load whatever the folder holds.
namespace
{

std::optional<std::filesystem::path> freePlugins()
{
    const auto folder = system::environmentVariable("SYMBOLENGINE_FREE_PLUGINS");
    if (!folder || folder->empty())
        return std::nullopt;
    return paths::fromUtf8(*folder);
}

struct Rig
{
    ScratchFolder scratch;
    TestMainThread main;
    Vst3Format vst3{{"SymbolEngine tests", main.post(), main.isCurrent()}};
#if defined(__APPLE__)
    AudioUnitFormat audioUnits{{main.post(), main.isCurrent(), nullptr}};
#endif
    PluginList list{{scratch.path / "Settings" / "Plugins.json", "1.0", nullptr, nullptr, nullptr}};

    // Every plugin file of the folder scanned in workers, as a program's scan does.
    void scan(const std::filesystem::path& folder)
    {
        std::vector<PluginScanFolder> folders{{&vst3, {folder / "VST3", false}}};
#if defined(__APPLE__)
        folders.push_back({&audioUnits, {folder / "Components", false}});
#endif
        PluginScanner scanner(list, PluginScanner::Options{
                                        {SYMBOLENGINE_SCAN_WORKER, {}, scratch.path / "workers.txt", 120s, main.post()},
                                        PluginSetup{48000.0, 512, false},
                                        50,
                                        std::int64_t(4) * 1024 * 1024 * 1024,
                                        10s});
        std::optional<PluginScanEnd> end;
        scanner.onFinished = [&](const PluginScanEnd& how) { end = how; };
        const auto walked = PluginScanner::walk(folders, list);
        REQUIRE(scanner.scan(walked.files));
        REQUIRE(main.turnUntil([&] { return end.has_value(); }, 600s));
        CHECK_FALSE(end->stopped);
        CHECK(end->problem.empty());
    }

    PluginFormat& formatOf(const PluginDescription& plugin)
    {
#if defined(__APPLE__)
        if (plugin.identity.format == audioUnits.name())
            return audioUnits;
#endif
        static_cast<void>(plugin);
        return vst3;
    }
};

// Plays a plugin as a program does, on an audio thread: blocks of noise of a size it was set up for, silence, and a
// short block. How loud the loudest sample that came out was; a sample that wasn't a number counts as infinitely so.
double play(Plugin& plugin, int frames)
{
    const auto layout = plugin.layout();
    const auto channels = std::max({layout.inputs, layout.outputs, 1});
    std::vector<std::vector<float>> buffers(static_cast<std::size_t>(channels),
                                            std::vector<float>(static_cast<std::size_t>(frames)));
    std::vector<float*> pointers;
    for (auto& buffer : buffers)
        pointers.push_back(buffer.data());

    double loudest = 0.0;
    std::thread audio([&] {
        std::mt19937 random(20261030);
        std::uniform_real_distribution<float> noise(-0.25f, 0.25f);
        const auto block = [&](int count, bool sound) {
            for (auto& buffer : buffers)
                for (auto& sample : buffer)
                    sample = sound ? noise(random) : 0.0f;
            plugin.process(pointers.data(), channels, count);
            for (const auto& buffer : buffers)
                for (int i = 0; i < count; ++i)
                {
                    const auto sample = static_cast<double>(buffer[static_cast<std::size_t>(i)]);
                    loudest = std::isfinite(sample) ? std::max(loudest, std::abs(sample)) : HUGE_VAL;
                }
        };
        for (int i = 0; i < 100; ++i)
            block(frames, true);
        for (int i = 0; i < 20; ++i)
            block(frames, false);
        block(std::min(frames, 7), true);
    });
    audio.join();
    return loudest;
}

}

TEST_CASE("Free plugins by other makers are scanned, each found as it declares itself and none set aside", "[free]")
{
    const auto folder = freePlugins();
    if (!folder)
        SKIP("SYMBOLENGINE_FREE_PLUGINS names no folder of them.");

    Rig rig;
    rig.scan(*folder);

    struct Expected
    {
        std::string format, name;
        PluginKind kind;
    };
    std::vector<Expected> expected{{"VST3", "Surge XT", PluginKind::instrument},
                                   {"VST3", "Surge XT Effects", PluginKind::effect},
                                   {"VST3", "Dragonfly Early Reflections", PluginKind::effect},
                                   {"VST3", "Dragonfly Hall Reverb", PluginKind::effect},
                                   {"VST3", "Dragonfly Plate Reverb", PluginKind::effect},
                                   {"VST3", "Dragonfly Room Reverb", PluginKind::effect}};
#if defined(__APPLE__)
    expected.push_back({"AudioUnit", "Surge XT", PluginKind::instrument});
    expected.push_back({"AudioUnit", "Surge XT Effects", PluginKind::effect});
#endif

    std::string all;
    const auto plugins = rig.list.plugins();
    for (const auto& entry : plugins)
        all += entry.plugin.identity.format + ": " + entry.plugin.name + " by " + entry.plugin.maker + "\n";
    INFO("Found:\n" << all);
    for (const auto& want : expected)
    {
        INFO(want.format << ": " << want.name);
        const auto found = std::find_if(plugins.begin(), plugins.end(), [&](const PluginEntry& entry) {
            return entry.plugin.identity.format == want.format && entry.plugin.name == want.name;
        });
        REQUIRE(found != plugins.end());
        const auto& plugin = found->plugin;
        CHECK(plugin.kind == want.kind);
        CHECK_FALSE(plugin.version.empty());
        CHECK_FALSE(plugin.maker.empty());
        if (want.kind == PluginKind::effect)
        {
            REQUIRE_FALSE(plugin.layouts.empty());
            CHECK(plugin.layouts[0].outputs > 0);
        }
        else
            CHECK(plugin.layouts.empty());

        const auto file = rig.list.file(plugin.location);
        REQUIRE(file);
        CHECK(file->outcome == PluginFile::Outcome::found);
        CHECK(file->setAside.empty());
    }
}

TEST_CASE("Free plugins by other makers are made, played at two rates, changed, saved and restored into a new one",
          "[free]")
{
    const auto folder = freePlugins();
    if (!folder)
        SKIP("SYMBOLENGINE_FREE_PLUGINS names no folder of them.");

    Rig rig;
    rig.scan(*folder);
    PluginHost host{{rig.main.post(), rig.main.isCurrent(), 5000ms, nullptr}};

    const auto make = [&](const PluginDescription& description) {
        auto plugin = host.make(rig.formatOf(description), description);
        REQUIRE(rig.main.turnUntil([&] { return plugin->state() != Plugin::State::making; }, 120s));
        INFO(plugin->problem().message);
        REQUIRE(plugin->state() == Plugin::State::inactive);
        return plugin;
    };

    int played = 0;
    for (const auto& entry : rig.list.plugins())
    {
        const auto& description = entry.plugin;
        if (description.kind != PluginKind::effect)
            continue;
        INFO(description.identity.format << ": " << description.name);
        auto plugin = make(description);

        // Each rate as a program moves between them, with its sound reaching the output.
        for (const auto rate : {48000.0, 44100.0})
        {
            INFO(rate);
            REQUIRE(plugin->prepare({rate, 512, false}));
            const auto loudest = play(*plugin, 512);
            CHECK(std::isfinite(loudest));
            CHECK(loudest > 0.0);
            CHECK(plugin->state() == Plugin::State::active);
            plugin->release();
            REQUIRE(rig.main.turnUntil([&] { return !plugin->isBusy(); }, 10s));
        }

        // A parameter moved far from where it began, which its state then has to carry into a new plugin.
        REQUIRE(plugin->prepare({48000.0, 512, false}));
        const auto parameters = plugin->parameters();
        const auto moved = std::find_if(parameters.begin(), parameters.end(), [](const PluginParameter& one) {
            return !one.readOnly && !one.hidden && !one.bypass && !one.programChange && one.automatable;
        });
        if (moved != parameters.end())
            CHECK(plugin->setParameter(moved->id, plugin->parameter(moved->id) > 0.5 ? 0.25 : 0.75));
        play(*plugin, 512);
        const auto state = plugin->saveState(PluginStateUse::document);
        REQUIRE(state);

        // Its values as it holds them, read from it by giving it what it just saved, as a plugin may keep a value to
        // steps of its own, which it needn't say.
        REQUIRE(plugin->loadState(*state, PluginStateUse::document));
        std::map<std::uint32_t, double> saved;
        for (const auto& parameter : parameters)
            saved[parameter.id] = plugin->parameter(parameter.id);
        plugin->release();
        REQUIRE(rig.main.turnUntil([&] { return !plugin->isBusy(); }, 10s));

        auto restored = make(description);
        REQUIRE(restored->loadState(*state, PluginStateUse::document));
        REQUIRE(restored->prepare({48000.0, 512, false}));
        CHECK(std::isfinite(play(*restored, 512)));
        for (const auto& parameter : parameters)
            if (!parameter.readOnly && !parameter.hidden)
            {
                INFO(parameter.name);
                const auto steps = parameter.steps > 0 ? 1.0 / parameter.steps : 0.02;
                CHECK(std::abs(restored->parameter(parameter.id) - saved[parameter.id]) <= steps);
            }
        if (moved != parameters.end())
        {
            INFO("Moved: " << moved->name << ", " << saved[moved->id] << " from " << moved->defaultValue);
            CHECK(std::abs(saved[moved->id] - moved->defaultValue) > 0.02);
        }
        restored->release();
        REQUIRE(rig.main.turnUntil([&] { return !restored->isBusy(); }, 10s));
        ++played;
    }
    CHECK(played > 0);
}
