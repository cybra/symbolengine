// A device's late blocks named by the plugin that made them late, the devices module's stream asking the host as a
// program joins the two.

#include <symbolengine/devices/AudioStream.h>
#include <symbolengine/devices/FakeAudioSystem.h>
#include <symbolengine/host/Plugin.h>
#include <symbolengine/host/Vst3Format.h>

#include "Helpers.h"
#include "TestMainThread.h"
#include "Vst3/TestIds.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <string>
#include <vector>

using namespace symbolengine;
using Kind = AudioEvent::Kind;

TEST_CASE("A plugin that sleeps 50 ms in every hundredth block is named in each late block's event, and a block made "
          "late by the program itself names no plugin",
          "[timing]")
{
    TestMainThread main;
    Vst3Format format{{"SymbolEngine tests", main.post(), main.isCurrent()}};
    PluginHost host{{main.post(), main.isCurrent(), 2000ms, nullptr}};
    const auto made = [&](const char* id) {
        auto plugin = host.make(format, Vst3Format::describe(SYMBOLENGINE_TEST_VST3, id));
        REQUIRE(main.turnUntil([&] { return plugin->state() != Plugin::State::making; }));
        REQUIRE(plugin->prepare({48000.0, 512, false}));
        return plugin;
    };
    const auto slow = made(testIds::faulting);
    const auto gain = made(testIds::gain);
    REQUIRE(slow->setParameter(testIds::slowInProcess, 1.0));

    // The program's callback, playing its input through the two in turn and then spending what it is told itself.
    struct Chain : AudioCallback
    {
        std::vector<Plugin*> plugins;
        std::atomic<std::int64_t> spend{0};

        void process(const AudioBlock& block) noexcept override
        {
            for (int c = 0; c < std::min(block.inputChannels, block.outputChannels); ++c)
                std::copy_n(block.inputs[c], block.frames, block.outputs[c]);

            for (auto* plugin : plugins)
                plugin->process(block.outputs, block.outputChannels, block.frames);

            const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(spend.load());
            while (std::chrono::steady_clock::now() < until)
            {
            }
        }
    };

    Chain chain;
    chain.plugins = {slow.get(), gain.get()};
    auto system = std::make_shared<FakeAudioSystem>();
    auto& device = system->add(fakeDevice("interface"));
    Events events;
    auto options = quick(events);
    // Each plugin known by its class ID, as both have the module's name here.
    options.describeLate = [&host](std::int64_t began, std::int64_t ended) {
        const auto late = host.slowestBetween(began, ended);
        return !late ? std::string()
                     : late->plugin.identity.id + (late->alone ? " took the whole block" : " took the most");
    };
    AudioStream stream(system, device.info().id, stereo(), options);
    REQUIRE(!stream.add(chain));
    REQUIRE(!stream.start());

    // Its hundredth and two hundredth blocks are late, the second told a second after the first.
    for (int i = 0; i < 250; ++i)
        REQUIRE(device.cycle());
    REQUIRE(events.waitFor(Kind::late, 2, 3000ms));
    for (const auto& late : events.of(Kind::late))
    {
        CHECK(late.count == 1);
        CHECK(late.nanoseconds >= 50'000'000);
        CHECK(late.cause == std::string(testIds::faulting) + " took the whole block");
    }

    // A block the program itself takes 30 ms over is late with no plugin to name.
    chain.spend = 30'000;
    REQUIRE(device.cycle());
    chain.spend = 0;
    REQUIRE(events.waitFor(Kind::late, 3, 3000ms));
    CHECK(events.of(Kind::late).back().count == 1);
    CHECK(events.of(Kind::late).back().cause.empty());
    CHECK(stream.statistics().overruns == 3);

    REQUIRE(!stream.stop());
}
