// What a plugin calls on the audio thread that a real-time thread never should, found by a program linked to what
// watches for it, in a test plugin built on its own that knows nothing of it.

#include <symbolengine/core/Paths.h>
#include <symbolengine/core/System.h>
#include <symbolengine/host/FakePluginFormat.h>
#include <symbolengine/host/Plugin.h>
#include <symbolengine/host/Vst3Format.h>

#include "TestMainThread.h"
#include "Vst3/TestIds.h"

#if defined(__APPLE__)
    #include <symbolengine/host/AudioUnitFormat.h>

    #include "AudioUnits/Registry.h"
#endif

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace symbolengine;
using namespace std::chrono_literals;
using Kind = PluginUnsafeCall::Kind;

namespace
{

struct Rig
{
    TestMainThread main;
    Vst3Format format{{"SymbolEngine tests", main.post(), main.isCurrent()}};
    PluginHost host;

    explicit Rig(bool watch = true) : host(options(watch)) {}

    PluginHost::Options options(bool watch)
    {
        PluginHost::Options made{main.post(), main.isCurrent(), 2000ms, nullptr};
        made.watchUnsafeCalls = watch;
        return made;
    }

    std::shared_ptr<Plugin> made(const char* id = testIds::unsafe)
    {
        auto plugin = host.make(format, Vst3Format::describe(SYMBOLENGINE_TEST_VST3, id));
        REQUIRE(main.turnUntil([&] { return plugin->state() != Plugin::State::making; }));
        REQUIRE(plugin->state() == Plugin::State::inactive);
        REQUIRE(plugin->prepare({48000.0, 512, false}));
        return plugin;
    }

    // Plays the plugins in turn on an audio thread of the test's own, as a program's device would.
    void play(const std::vector<std::shared_ptr<Plugin>>& plugins, int blocks)
    {
        std::thread audio([&] {
            std::vector<float> left(512), right(512);
            float* channels[] = {left.data(), right.data()};

            for (int i = 0; i < blocks; ++i)
                for (const auto& plugin : plugins)
                    plugin->process(channels, 2, 512);
        });
        audio.join();
    }

    void play(const std::shared_ptr<Plugin>& plugin, int blocks) { play(std::vector{plugin}, blocks); }

    // Gets past the first calls the system binds and what the plugin does setting itself up, tells it what to do from
    // its next block on, plays, and says what was found from then.
    std::vector<PluginUnsafeCall> found(testIds::UnsafeDoes does, int blocks = 20)
    {
        const auto plugin = made();
        play(plugin, 5);
        const auto before = plugin->unsafeCalls().size();
        REQUIRE(plugin->setParameter(testIds::unsafeDoes, testIds::unsafeValue(does)));
        play(plugin, blocks);
        auto calls = plugin->unsafeCalls();
        calls.erase(calls.begin(), calls.begin() + static_cast<std::ptrdiff_t>(before));
        return calls;
    }
};

// Everything below needs what watches linked in, which a sanitizer's own interception of the same calls gets in the way
// of. The tests of a build with sanitizers end here, having checked that a program that can't watch isn't harmed.
#define NEEDS_WATCHING()                                                                                               \
    if (!PluginHost::canWatchUnsafeCalls())                                                                            \
    SKIP("something else in this process takes these calls first")

bool anyOf(const std::vector<PluginUnsafeCall>& calls, Kind kind)
{
    return std::any_of(calls.begin(), calls.end(), [&](const auto& call) { return call.kind == kind; });
}

// Whether a call's function has any of these in its name, as each system names the same thing its own way.
bool anyNamed(const std::vector<PluginUnsafeCall>& calls, const std::vector<std::string>& parts)
{
    return std::any_of(calls.begin(), calls.end(), [&](const auto& call) {
        return std::any_of(parts.begin(), parts.end(),
                           [&](const auto& part) { return call.function.find(part) != std::string::npos; });
    });
}

// What each call's way there says, for a failing test to show.
std::string said(const std::vector<PluginUnsafeCall>& calls)
{
    std::string text;

    for (const auto& call : calls)
    {
        text += call.function + " in " + call.step + (call.inPluginCode ? " (plugin)" : " (not plugin)") + " x" +
                std::to_string(call.times) + " blocks " + std::to_string(call.firstBlock) + ".." +
                std::to_string(call.lastBlock) + "\n";

        for (const auto& frame : call.backtrace)
            text += "    " + frame + "\n";
    }

    return text;
}

// That the way to the call goes through the test plugin's bundle.
bool throughTestPlugin(const PluginUnsafeCall& call)
{
    return std::any_of(call.backtrace.begin(), call.backtrace.end(),
                       [](const auto& frame) { return frame.find("SymbolEngine Test") != std::string::npos; });
}

}

TEST_CASE("This program can watch for unsafe calls")
{
#if defined(SYMBOLENGINE_SANITIZED)
    SKIP("a sanitizer takes these calls first");
#else
    CHECK(PluginHost::canWatchUnsafeCalls());
#endif
}

TEST_CASE("A plugin that makes no unsafe call is found to make none, nor is the host's own way into it")
{
    NEEDS_WATCHING();
    Rig rig;
    const auto plugin = rig.made();
    rig.play(plugin, 200);
    const auto calls = plugin->unsafeCalls();
    INFO(said(calls));
    CHECK(plugin->timing().blocks == 200);
    // The first blocks may bind what they call for the first time, which isn't the plugin's doing: past them, nothing.
    for (const auto& call : calls)
        CHECK(call.lastBlock < 5);
    CHECK(plugin->unsafeCallsNotListed() == 0);
}

TEST_CASE("Memory taken in a plugin's process call is found, named and traced into the plugin")
{
    NEEDS_WATCHING();
    Rig rig;

    SECTION("through the C library")
    {
        const auto calls = rig.found(testIds::doesMalloc);
        INFO(said(calls));
        REQUIRE(calls.size() >= 2);
        CHECK(anyOf(calls, Kind::memory));

        for (const auto& call : calls)
        {
            CHECK(call.kind == Kind::memory);
            CHECK(call.step == "process");
            CHECK(call.inPluginCode);
            CHECK(call.times == 20);
            CHECK(call.blocks == 20);
            CHECK(call.firstBlock == 5);
            CHECK(call.lastBlock == 24);
            CHECK(throughTestPlugin(call));

            // Its way there stops where the plugin's own code does, past which is only the host's way into it.
            REQUIRE_FALSE(call.backtrace.empty());
            CHECK(call.backtrace.back().find("SymbolEngine Test") != std::string::npos);
        }

        // Taking it and giving it back are each found, whatever each system calls them.
        const auto gaveBack = std::any_of(calls.begin(), calls.end(), [](const auto& call) {
            return call.function.find("free") != std::string::npos || call.function.find("Free") != std::string::npos;
        });
        CHECK(gaveBack);
    }

    SECTION("through new")
    {
        const auto calls = rig.found(testIds::doesNew);
        INFO(said(calls));
        REQUIRE_FALSE(calls.empty());
        CHECK(throughTestPlugin(calls.front()));

        // Once each for taking and giving back, whatever new and delete go through on the way to the allocator.
        CHECK(std::count_if(calls.begin(), calls.end(), [](const auto& call) { return call.kind == Kind::memory; }) ==
              2);
    }

    SECTION("through a vector growing in the plugin's own code")
    {
        const auto calls = rig.found(testIds::doesVector);
        INFO(said(calls));
        REQUIRE_FALSE(calls.empty());
        CHECK(anyOf(calls, Kind::memory));
    }

    SECTION("through a string growing inside the C++ library, a system library's code calling for it")
    {
        const auto calls = rig.found(testIds::doesString);
        INFO(said(calls));
        REQUIRE_FALSE(calls.empty());
        CHECK(anyOf(calls, Kind::memory));
        CHECK(calls.front().inPluginCode);
    }

    SECTION("through what this system's own libraries allocate with")
    {
        const auto calls = rig.found(testIds::doesSystemAllocation);
        INFO(said(calls));
        REQUIRE_FALSE(calls.empty());
        CHECK(calls.front().inPluginCode);

        // Taking it in every block, not only giving it back: on a Mac CoreFoundation's own way, through its zones.
        CHECK(std::any_of(calls.begin(), calls.end(), [](const auto& call) {
            const auto& name = call.function;
            return call.blocks == 20 &&
                   (name.find("alloc") != std::string::npos || name.find("Alloc") != std::string::npos) &&
                   name.find("free") == std::string::npos && name.find("Free") == std::string::npos;
        }));
    }
}

TEST_CASE("Locks, waits, sleeps, files and threads in a plugin's process call are found, each as what it is")
{
    NEEDS_WATCHING();
    Rig rig;

    SECTION("a mutex")
    {
        const auto calls = rig.found(testIds::doesLock);
        INFO(said(calls));
        REQUIRE_FALSE(calls.empty());
        CHECK(anyOf(calls, Kind::lock));
        CHECK(calls.front().inPluginCode);
    }

    SECTION("only trying a lock, which never waits, isn't found")
    {
        const auto calls = rig.found(testIds::doesTryLock);
        INFO(said(calls));
        CHECK_FALSE(anyOf(calls, Kind::lock));
    }

    SECTION("a lock of the system's own kind")
    {
        const auto calls = rig.found(testIds::doesSystemLock);
        INFO(said(calls));
        CHECK(anyOf(calls, Kind::lock));
        CHECK(anyNamed(calls, {"unfair", "SRW", "spin"}));
    }

    SECTION("a read-write lock")
    {
        const auto calls = rig.found(testIds::doesReadWriteLock);
        INFO(said(calls));
        CHECK(anyOf(calls, Kind::lock));
        CHECK(anyNamed(calls, {"rwlock", "SRW"}));
    }

    SECTION("a wait on a condition")
    {
        const auto calls = rig.found(testIds::doesConditionWait);
        INFO(said(calls));
        CHECK(anyOf(calls, Kind::lock));

        // The wait itself, as the lock held for it would be found anyway.
        CHECK(anyNamed(calls, {"cond", "Condition"}));
    }

    SECTION("a sleep")
    {
        const auto calls = rig.found(testIds::doesSleep);
        INFO(said(calls));
        CHECK(anyOf(calls, Kind::sleep));
    }

    SECTION("a file")
    {
        const auto calls = rig.found(testIds::doesFile);
        INFO(said(calls));
        CHECK(anyOf(calls, Kind::file));
    }

    SECTION("a thread")
    {
        const auto calls = rig.found(testIds::doesThread);
        INFO(said(calls));
        CHECK(anyOf(calls, Kind::thread));
    }
}

TEST_CASE("Giving back no memory, which costs nothing, isn't found")
{
    NEEDS_WATCHING();
    Rig rig;
    const auto calls = rig.found(testIds::doesFreeNothing);
    INFO(said(calls));
    CHECK(calls.empty());
}

TEST_CASE("Unsafe calls are held against the plugin that made them, and no other")
{
    NEEDS_WATCHING();
    Rig rig;
    const auto clean = rig.made(testIds::unsafe);
    const auto guilty = rig.made(testIds::unsafeTwin);
    const auto alsoClean = rig.made(testIds::unsafe);
    rig.play({clean, guilty, alsoClean}, 5);
    const auto cleanBefore = clean->unsafeCalls().size();
    const auto alsoBefore = alsoClean->unsafeCalls().size();
    const auto guiltyBefore = guilty->unsafeCalls().size();
    REQUIRE(guilty->setParameter(testIds::unsafeDoes, testIds::unsafeValue(testIds::doesMalloc)));
    rig.play({clean, guilty, alsoClean}, 20);

    INFO(said(clean->unsafeCalls()));
    INFO(said(guilty->unsafeCalls()));
    CHECK(clean->unsafeCalls().size() == cleanBefore);
    CHECK(alsoClean->unsafeCalls().size() == alsoBefore);
    CHECK(guilty->unsafeCalls().size() > guiltyBefore);
}

TEST_CASE("Only calls on the audio thread in a plugin's own call are found")
{
    NEEDS_WATCHING();
    Rig rig;

    SECTION("what its worker thread does during its call isn't")
    {
        const auto calls = rig.found(testIds::doesWorkerAllocation);
        INFO(said(calls));
        const auto later = std::count_if(calls.begin(), calls.end(), [](const auto& call) { return call.blocks > 1; });
        CHECK(later == 0);
    }

    SECTION("an offline render isn't, as nothing is waiting for it and a plugin may wait there, and playing again is")
    {
        const auto plugin = rig.made();
        REQUIRE(plugin->prepare({48000.0, 512, true}));
        REQUIRE(plugin->setParameter(testIds::unsafeDoes, testIds::unsafeValue(testIds::doesMalloc)));
        rig.play(plugin, 20);
        INFO(said(plugin->unsafeCalls()));
        CHECK(plugin->unsafeCalls().empty());

        REQUIRE(plugin->prepare({48000.0, 512, false}));
        rig.play(plugin, 20);
        CHECK(anyOf(plugin->unsafeCalls(), Kind::memory));
    }

    SECTION("what the plugin does on the main thread isn't, as being made and prepared allocate plenty")
    {
        const auto plugin = rig.made();
        CHECK(plugin->unsafeCalls().empty());
        REQUIRE(plugin->saveState(PluginStateUse::document).has_value());
        CHECK(plugin->unsafeCalls().empty());
    }

    SECTION("what the program does between a plugin's calls, on the same thread, isn't")
    {
        const auto plugin = rig.made();
        rig.play(plugin, 5);
        const auto before = plugin->unsafeCalls().size();
        std::thread audio([&] {
            std::vector<float> left(512), right(512);
            float* channels[] = {left.data(), right.data()};

            for (int i = 0; i < 20; ++i)
            {
                plugin->process(channels, 2, 512);
                std::vector<int> ownWork(1000);
                std::this_thread::sleep_for(1us);
            }
        });
        audio.join();
        INFO(said(plugin->unsafeCalls()));
        CHECK(plugin->unsafeCalls().size() == before);
    }
}

TEST_CASE("A plugin that sets itself up in its first block can be told from one that goes on")
{
    NEEDS_WATCHING();
    Rig rig;

    SECTION("only in the first block from then on")
    {
        const auto calls = rig.found(testIds::doesMallocOnce);
        INFO(said(calls));
        REQUIRE_FALSE(calls.empty());

        for (const auto& call : calls)
        {
            CHECK(call.times == 1);
            CHECK(call.blocks == 1);
            CHECK(call.firstBlock == 5);
            CHECK(call.lastBlock == 5);
        }
    }

    SECTION("in every tenth block")
    {
        const auto calls = rig.found(testIds::doesMallocEveryTenth, 100);
        INFO(said(calls));
        REQUIRE_FALSE(calls.empty());

        for (const auto& call : calls)
        {
            CHECK(call.times == 10);
            CHECK(call.blocks == 10);
            CHECK(call.firstBlock == 5);
            CHECK(call.lastBlock == 95);
        }
    }

    SECTION("twice in every block, its blocks counted apart from its calls")
    {
        const auto calls = rig.found(testIds::doesMallocTwice);
        INFO(said(calls));
        REQUIRE_FALSE(calls.empty());

        for (const auto& call : calls)
        {
            CHECK(call.times == 40);
            CHECK(call.blocks == 20);
        }
    }

    SECTION("as its processing starts, said as that")
    {
        const auto plugin = rig.made();
        REQUIRE(plugin->setParameter(testIds::unsafeAtStart, testIds::unsafeValue(testIds::doesMalloc)));
        rig.play(plugin, 3);
        const auto calls = plugin->unsafeCalls();
        INFO(said(calls));
        const auto atStart =
            std::find_if(calls.begin(), calls.end(), [](const auto& call) { return call.step == "startProcessing"; });
        REQUIRE(atStart != calls.end());
        CHECK(atStart->kind == Kind::memory);
        CHECK(atStart->inPluginCode);
    }
}

TEST_CASE("The program is told once for each new place, on the main thread, however many times the call is made")
{
    NEEDS_WATCHING();
    Rig rig;
    const auto plugin = rig.made();
    rig.play(plugin, 5);
    rig.main.settle();
    int told = 0;
    bool onMain = true;
    plugin->onChange = [&](PluginChange change) {
        if (change == PluginChange::unsafeCall)
        {
            ++told;
            onMain = onMain && rig.main.isCurrent()();
        }
    };
    REQUIRE(plugin->setParameter(testIds::unsafeDoes, testIds::unsafeValue(testIds::doesMalloc)));
    rig.play(plugin, 50);
    CHECK(rig.main.turnUntil([&] { return told > 0; }));
    CHECK(onMain);
    const auto first = told;
    rig.main.settle();
    rig.play(plugin, 50);
    std::this_thread::sleep_for(50ms);
    rig.main.settle();
    CHECK(told == first);
}

TEST_CASE("A plugin's list holds what it can and counts the rest, the audio playing on")
{
    NEEDS_WATCHING();
    Rig rig;
    const auto plugin = rig.made();
    rig.play(plugin, 5);
    REQUIRE(plugin->setParameter(testIds::unsafeDoes, testIds::unsafeValue(testIds::doesManyPlaces)));
    rig.play(plugin, 3);
    CHECK(plugin->unsafeCalls().size() == 64);
    CHECK(plugin->unsafeCallsNotListed() > 0);
    CHECK(plugin->timing().blocks == 8);
}

TEST_CASE("A host not asked to watch finds nothing and is told nothing")
{
    Rig rig(false);
    const auto plugin = rig.made();
    bool told = false;
    plugin->onChange = [&](PluginChange change) { told = told || change == PluginChange::unsafeCall; };
    rig.play(plugin, 2);
    REQUIRE(plugin->setParameter(testIds::unsafeDoes, testIds::unsafeValue(testIds::doesMalloc)));
    rig.play(plugin, 20);
    rig.main.settle();
    CHECK(plugin->unsafeCalls().empty());
    CHECK(plugin->unsafeCallsNotListed() == 0);
    CHECK_FALSE(told);
}

TEST_CASE("The list can be read while the plugin plays")
{
    NEEDS_WATCHING();
    Rig rig;
    const auto plugin = rig.made();
    rig.play(plugin, 2);
    REQUIRE(plugin->setParameter(testIds::unsafeDoes, testIds::unsafeValue(testIds::doesManyPlaces)));
    std::atomic<bool> done{false};
    std::thread audio([&] {
        std::vector<float> left(512), right(512);
        float* channels[] = {left.data(), right.data()};

        for (int i = 0; i < 40; ++i)
            plugin->process(channels, 2, 512);

        done = true;
    });
    std::size_t most = 0;

    while (!done)
    {
        const auto calls = plugin->unsafeCalls();
        CHECK(calls.size() >= most);
        most = calls.size();

        for (const auto& call : calls)
            CHECK_FALSE(call.function.empty());
    }

    audio.join();
    CHECK(most > 0);
}

TEST_CASE("A call with no plugin code on the way to it is said to be the program's or the system's")
{
    NEEDS_WATCHING();
    TestMainThread main;
    FakePluginFormat fake{main.isCurrent(), main.post()};
    PluginHost::Options options{main.post(), main.isCurrent(), 2000ms, nullptr};
    options.watchUnsafeCalls = true;
    PluginHost host{options};
    FakePluginFormat::Script script;
    script.onBlock = [](const PluginBlock&) {
        void* volatile memory = std::malloc(64);
        std::free(memory);
    };
    fake.script("inTheProgram", script);
    const auto plugin = host.make(fake, FakePluginFormat::describe("inTheProgram"));
    REQUIRE(main.turnUntil([&] { return plugin->state() != Plugin::State::making; }));
    REQUIRE(plugin->prepare({48000.0, 512, false}));
    std::thread audio([&] {
        std::vector<float> left(512), right(512);
        float* channels[] = {left.data(), right.data()};

        for (int i = 0; i < 5; ++i)
            plugin->process(channels, 2, 512);
    });
    audio.join();
    const auto calls = plugin->unsafeCalls();
    INFO(said(calls));
    REQUIRE_FALSE(calls.empty());

    for (const auto& call : calls)
    {
        CHECK_FALSE(call.inPluginCode);
        CHECK_FALSE(call.backtrace.empty());
    }
}

#if defined(__APPLE__)

TEST_CASE("An Audio Unit's calls are held against it, and what the system does on the way into one is told apart")
{
    NEEDS_WATCHING();
    const std::filesystem::path units = SYMBOLENGINE_TEST_AUDIO_UNITS;
    REQUIRE(testAudioUnits::registerAll(units));
    TestMainThread main;
    AudioUnitFormat format{{main.post(), main.isCurrent(), nullptr}};
    PluginHost::Options options{main.post(), main.isCurrent(), 2000ms, nullptr};
    options.watchUnsafeCalls = true;
    PluginHost host{options};

    const auto made = [&](const char* subtype) {
        PluginError error;
        const auto code = [](const char* text) {
            return (std::uint32_t(std::uint8_t(text[0])) << 24) | (std::uint32_t(std::uint8_t(text[1])) << 16) |
                   (std::uint32_t(std::uint8_t(text[2])) << 8) | std::uint32_t(std::uint8_t(text[3]));
        };
        const auto identity = PluginIdentity::audioUnit(code("aufx"), code(subtype), code("SyTs"));

        for (const auto& found : format.pluginsIn(units, error))
            if (found.identity == identity)
            {
                auto plugin = host.make(format, found);
                REQUIRE(main.turnUntil([&] { return plugin->state() != Plugin::State::making; }));
                REQUIRE(plugin->state() == Plugin::State::inactive);
                REQUIRE(plugin->prepare({48000.0, 512, false}));
                return plugin;
            }

        FAIL("no test unit " << subtype);
        return std::shared_ptr<Plugin>{};
    };
    const auto play = [](const std::shared_ptr<Plugin>& plugin, int blocks) {
        std::thread audio([&] {
            std::vector<float> left(512), right(512);
            float* channels[] = {left.data(), right.data()};

            for (int i = 0; i < blocks; ++i)
                plugin->process(channels, 2, 512);
        });
        audio.join();
    };

    SECTION("a unit that makes none has none held against it, past what the first blocks bind")
    {
        const auto plugin = made("Pass");
        play(plugin, 100);
        const auto calls = plugin->unsafeCalls();
        INFO(said(calls));
        CHECK(plugin->timing().blocks == 100);

        for (const auto& call : calls)
            CHECK(call.lastBlock < 5);
    }

    SECTION("a unit that takes memory in its render is found")
    {
        const auto plugin = made("Unsf");
        play(plugin, 20);
        const auto calls = plugin->unsafeCalls();
        INFO(said(calls));
        const auto ownCall = std::find_if(calls.begin(), calls.end(), [](const auto& call) {
            return call.kind == Kind::memory && call.inPluginCode && call.firstBlock == 5;
        });
        REQUIRE(ownCall != calls.end());
        CHECK(ownCall->blocks == 15);
        CHECK(ownCall->step == "process");
        CHECK(ownCall->backtrace.back().find("SymbolEngine Test Units") != std::string::npos);
    }
}

#endif

// Plugins by other makers, which continuous integration downloads and names the folder of in
// SYMBOLENGINE_FREE_PLUGINS. Nothing can be said of what they ought to be found calling, only that playing on while
// watched does them no harm; what was found is written out, to be read.
TEST_CASE("Plugins by other makers play on while watched, saying what each was found calling", "[free]")
{
    const auto folder = system::environmentVariable("SYMBOLENGINE_FREE_PLUGINS");

    if (!folder || folder->empty())
        SKIP("SYMBOLENGINE_FREE_PLUGINS names no folder of them.");

    NEEDS_WATCHING();
    Rig rig;
    int played = 0;

    for (const auto& file : std::filesystem::directory_iterator(paths::fromUtf8(*folder) / "VST3"))
    {
        PluginError error;

        for (const auto& description : rig.format.pluginsIn(file.path(), error))
        {
            if (description.kind != PluginKind::effect)
                continue;

            INFO(description.name);
            const auto plugin = rig.host.make(rig.format, description);
            REQUIRE(rig.main.turnUntil([&] { return plugin->state() != Plugin::State::making; }, 120s));
            REQUIRE(plugin->state() == Plugin::State::inactive);
            REQUIRE(plugin->prepare({48000.0, 512, false}));

            std::thread audio([&] {
                std::mt19937 random(1);
                std::uniform_real_distribution<float> noise(-0.25f, 0.25f);
                std::vector<float> left(512), right(512);
                float* channels[] = {left.data(), right.data()};

                for (int i = 0; i < 500; ++i)
                {
                    for (int f = 0; f < 512; ++f)
                    {
                        left[static_cast<std::size_t>(f)] = noise(random);
                        right[static_cast<std::size_t>(f)] = noise(random);
                    }

                    plugin->process(channels, 2, 512);
                }
            });
            audio.join();

            CHECK(plugin->state() == Plugin::State::active);
            CHECK(plugin->timing().blocks == 500);
            const auto calls = plugin->unsafeCalls();
            const auto last = std::count_if(calls.begin(), calls.end(),
                                            [](const auto& call) { return call.inPluginCode && call.lastBlock >= 5; });
            std::printf("=== %s: %d places, %d of its own past its first blocks, %lld more past those listed\n%s\n",
                        description.name.c_str(), static_cast<int>(calls.size()), static_cast<int>(last),
                        static_cast<long long>(plugin->unsafeCallsNotListed()), said(calls).c_str());
            ++played;
        }
    }

    CHECK(played > 0);
}
