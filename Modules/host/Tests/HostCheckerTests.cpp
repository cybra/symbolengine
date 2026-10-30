#include "Scratch.h"
#include "TestMainThread.h"

#include <symbolengine/core/Files.h>
#include <symbolengine/core/Text.h>
#include <symbolengine/host/Plugin.h>
#include <symbolengine/host/Vst3Format.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <string>
#include <thread>
#include <vector>

using namespace symbolengine;
using namespace std::chrono_literals;

namespace
{

// As the checker's controller numbers them, from its own header.
constexpr std::uint32_t loadParameter = 1000, bypassParameter = 1003;

// Plays blocks of the size on an audio thread, each with a change to a parameter and where the program is in time,
// running on from where it is told.
void play(Plugin& plugin, int frames, int blocks, std::int64_t& position)
{
    std::vector<float> left(static_cast<std::size_t>(frames)), right(static_cast<std::size_t>(frames));
    float* channels[] = {left.data(), right.data()};
    std::thread audio([&] {
        for (int block = 0; block < blocks; ++block)
        {
            const PluginParameterChange change{loadParameter, frames / 2, block % 2 == 0 ? 0.25 : 0.0};
            PluginTime time;
            time.playing = true;
            time.position = position;
            time.tempo = 120.0;
            time.numerator = 4;
            time.denominator = 4;
            time.systemNanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now().time_since_epoch())
                                         .count();
            plugin.process(channels, 2, frames, &change, 1, &time);
            position += frames;
        }
    });
    audio.join();
}

}

TEST_CASE("Steinberg's host checker finds no fault through a plugin's whole life")
{
    ScratchFolder folder;
    const auto log = folder.path / "checker.txt";
#if defined(_WIN32)
    _putenv_s("SYMBOLENGINE_HOST_CHECKER_LOG", log.string().c_str());
#else
    setenv("SYMBOLENGINE_HOST_CHECKER_LOG", log.c_str(), 1);
#endif

    TestMainThread main;
    Vst3Format format{{"SymbolEngine tests", main.post(), main.isCurrent()}};
    std::vector<std::string> wrong;
    PluginHost host{{main.post(), main.isCurrent(), 2000ms, [&](const std::string& what) { wrong.push_back(what); }}};

    PluginError error;
    const auto found = format.pluginsIn(SYMBOLENGINE_HOST_CHECKER, error);
    REQUIRE_FALSE(error);
    REQUIRE(found.size() == 1);

    const auto make = [&] {
        auto plugin = host.make(format, found[0]);
        REQUIRE(main.turnUntil([&] { return plugin->state() != Plugin::State::making; }));
        INFO(plugin->problem().message);
        REQUIRE(plugin->state() == Plugin::State::inactive);
        return plugin;
    };

    std::int64_t position = 0;
    {
        // Loaded, played, then the rate and the block size changed as a program changes them.
        auto plugin = make();
        REQUIRE(plugin->prepare({48000.0, 512, false}));
        play(*plugin, 512, 50, position);
        REQUIRE(plugin->prepare({44100.0, 512, false}));
        play(*plugin, 512, 50, position);
        REQUIRE(plugin->prepare({44100.0, 128, false}));
        for (const int frames : {128, 1, 7, 64, 127})
            play(*plugin, frames, 20, position);

        // Switched off and on, its values changed, a jump in time, and its state saved and given back.
        plugin->release();
        REQUIRE(main.turnUntil([&] { return !plugin->isBusy(); }));
        REQUIRE(plugin->setParameter(bypassParameter, 1.0));
        REQUIRE(plugin->prepare({48000.0, 256, false}));
        play(*plugin, 256, 20, position);
        REQUIRE(plugin->setParameter(bypassParameter, 0.0));
        play(*plugin, 256, 20, position);
        position += 48000;
        play(*plugin, 256, 20, position);
        const auto state = plugin->saveState(PluginStateUse::document);
        REQUIRE(state);
        REQUIRE(plugin->loadState(*state, PluginStateUse::document));
        play(*plugin, 256, 20, position);
        plugin->release();
        REQUIRE(main.turnUntil([&] { return !plugin->isBusy(); }));

        // A copy of it given that state and rendered offline.
        auto copy = make();
        REQUIRE(copy->loadState(*state, PluginStateUse::copy));
        REQUIRE(copy->prepare({48000.0, 1024, true}));
        std::int64_t rendered = 0;
        play(*copy, 1024, 20, rendered);
        copy->release();
        REQUIRE(main.turnUntil([&] { return !copy->isBusy(); }));
    }

    // Its messages to its controller, and the values it reports, come on the main thread.
    main.turnUntil([] { return false; }, 200ms);
    main.settle();

    std::error_code missing;
    const auto lines = files::read(log, missing);
    REQUIRE_FALSE(missing);
    int total = 0;
    std::map<std::string, std::vector<std::string>> bySeverity;
    for (const auto line : text::split(lines, "\n"))
        if (!line.empty())
        {
            ++total;
            const std::string whole(line);
            bySeverity[whole.substr(0, whole.find('|'))].push_back(whole);
        }
    std::string shown;
    for (const auto& [severity, entries] : bySeverity)
        for (const auto& entry : entries)
            shown += entry + "\n";
    INFO(shown);

    // Its notes of what a host offers are many, so a checker that logged nothing has checked nothing.
    CHECK(total > 0);
    CHECK(bySeverity["Error"].empty());
    CHECK(bySeverity["Warn"].empty());
    CHECK(wrong.empty());
}
