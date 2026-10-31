#include <symbolengine/host/FakePluginFormat.h>
#include <symbolengine/host/Plugin.h>

#include "TestMainThread.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cfenv>
#include <clocale>
#include <cmath>
#include <filesystem>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace symbolengine;
using namespace std::chrono_literals;

namespace
{

// A host on the test's main thread with the fake format, recording what each plugin says and what the program did
// wrong.
struct Rig
{
    TestMainThread main;
    FakePluginFormat fake{main.isCurrent(), main.post()};
    std::vector<std::string> wrong;
    PluginHost host{
        {main.post(), main.isCurrent(), 2000ms, [this](const std::string& what) { wrong.push_back(what); }}};

    struct Heard
    {
        std::vector<PluginChange> changes;

        bool has(PluginChange change) const
        {
            return std::find(changes.begin(), changes.end(), change) != changes.end();
        }
    };

    std::shared_ptr<Plugin> make(const std::string& id, Heard* heard = nullptr)
    {
        auto plugin = host.make(fake, FakePluginFormat::describe(id));

        if (heard != nullptr)
            plugin->onChange = [heard](PluginChange change) { heard->changes.push_back(change); };

        return plugin;
    }

    std::shared_ptr<Plugin> made(const std::string& id, Heard* heard = nullptr)
    {
        auto plugin = make(id, heard);
        REQUIRE(main.turnUntil([&] { return plugin->state() != Plugin::State::making; }));
        return plugin;
    }

    void checkNoMisuse()
    {
        const auto misuses = fake.misuses();
        INFO((misuses.empty() ? std::string() : misuses.front()));
        CHECK(misuses.empty());
    }
};

// Plays blocks through a plugin from a thread of its own, as a device's audio thread does, until stopped.
struct AudioThread
{
    std::atomic<bool> running{true};
    std::atomic<int> blocks{0};
    std::thread thread;

    explicit AudioThread(std::shared_ptr<Plugin> plugin, int frames = 64)
        : thread([this, plugin, frames] {
              std::vector<float> left(static_cast<std::size_t>(frames), 0.25f), right = left;
              float* channels[] = {left.data(), right.data()};

              while (running)
              {
                  plugin->process(channels, 2, frames);
                  ++blocks;
                  std::this_thread::sleep_for(100us);
              }
          })
    {
    }

    ~AudioThread()
    {
        running = false;
        thread.join();
    }
};

std::vector<float> through(Plugin& plugin, int channels, int frames, float value = 1.0f)
{
    std::vector<std::vector<float>> audio(static_cast<std::size_t>(channels),
                                          std::vector<float>(static_cast<std::size_t>(frames), value));
    std::vector<float*> pointers;

    for (int c = 0; c < channels; ++c)
    {
        for (int i = 0; i < frames; ++i)
            audio[static_cast<std::size_t>(c)][static_cast<std::size_t>(i)] = value * static_cast<float>(c + 1);

        pointers.push_back(audio[static_cast<std::size_t>(c)].data());
    }

    plugin.process(pointers.data(), channels, frames);
    std::vector<float> lasts;

    for (const auto& channel : audio)
        lasts.push_back(channel.back());

    return lasts;
}

}

TEST_CASE("Plugins are made on the main thread one per turn of its run loop, in order")
{
    Rig rig;
    std::vector<std::string> order;
    std::vector<std::shared_ptr<Plugin>> plugins;

    for (const std::string id : {"a", "b", "c"})
    {
        plugins.push_back(rig.make(id));
        plugins.back()->onChange = [&order, id](PluginChange change) {
            if (change == PluginChange::made)
                order.push_back(id);
        };
    }

    CHECK(order.empty());

    const auto madeSoFar = [&plugins] {
        return std::count_if(plugins.begin(), plugins.end(),
                             [](const auto& plugin) { return plugin->state() != Plugin::State::making; });
    };

    for (long turn = 1; turn <= 3; ++turn)
    {
        // What else the main thread has to do, such as drawing a window, runs between them.
        bool drew = false;
        rig.main.post()([&drew] { drew = true; });
        rig.main.turn();

        CHECK(drew);
        CHECK(madeSoFar() == turn);
    }

    rig.main.settle();
    CHECK(order == std::vector<std::string>{"a", "b", "c"});

    for (const auto& plugin : plugins)
        CHECK(plugin->state() == Plugin::State::inactive);

    rig.checkNoMisuse();
    CHECK(rig.wrong.empty());
}

TEST_CASE("A plugin made asynchronously holds up only the plugins after it, never the main thread")
{
    Rig rig;
    FakePluginFormat::Script slow;
    slow.turnsToMake = 3;
    rig.fake.script("slow", slow);
    auto first = rig.make("slow");
    auto second = rig.make("quick");

    for (int turn = 0; turn < 3; ++turn)
    {
        rig.main.turn();
        CHECK(first->state() == Plugin::State::making);
        CHECK(second->state() == Plugin::State::making);
        CHECK(rig.fake.calls("quick").empty());
    }

    REQUIRE(rig.main.turnUntil([&] { return second->state() == Plugin::State::inactive; }));
    CHECK(first->state() == Plugin::State::inactive);
    rig.checkNoMisuse();
}

TEST_CASE("A plugin that can't be made says why, and those after it are made")
{
    Rig rig;
    FakePluginFormat::Script broken;
    broken.failToMake = true;
    rig.fake.script("broken", broken);
    Rig::Heard heard;
    auto failed = rig.made("broken", &heard);
    auto fine = rig.made("fine");

    CHECK(failed->state() == Plugin::State::failed);
    CHECK(failed->problem());
    CHECK(heard.has(PluginChange::made));
    CHECK(fine->state() == Plugin::State::inactive);
}

TEST_CASE("A plugin's layout is settled: stereo, else mono in and stereo out, else mono, else its own")
{
    Rig rig;
    const auto takes = [&rig](const std::string& id, std::vector<PluginLayout> layouts) {
        FakePluginFormat::Script script;
        script.takes = std::move(layouts);
        rig.fake.script(id, script);
    };

    takes("stereo", {{2, 2}, {1, 1}});
    takes("mono", {{1, 1}});
    takes("mono to stereo", {{1, 2}, {1, 1}});
    takes("eight", {{8, 8}});
    takes("no audio out", {});

    CHECK(rig.made("stereo")->layout() == PluginLayout{2, 2});
    CHECK(rig.made("mono")->layout() == PluginLayout{1, 1});
    CHECK(rig.made("mono to stereo")->layout() == PluginLayout{1, 2});
    CHECK(rig.made("eight")->layout() == PluginLayout{8, 8});

    auto none = rig.made("no audio out");
    CHECK(none->state() == Plugin::State::failed);
    CHECK(none->problem().message == "It gives no audio out.");
    rig.checkNoMisuse();
}

TEST_CASE("A plugin passes the audio as it came until it is prepared, then plays it, fitted to the program's channels")
{
    Rig rig;
    FakePluginFormat::Script half;
    half.gain = 0.5f;
    half.latency = 10;
    half.tail = 20;
    rig.fake.script("half", half);
    FakePluginFormat::Script mono = half;
    mono.takes = {{1, 1}};
    rig.fake.script("mono", mono);

    auto plugin = rig.made("half");
    CHECK(through(*plugin, 2, 100) == std::vector<float>{1.0f, 2.0f});

    REQUIRE(plugin->prepare({48000.0, 64, false}));
    CHECK(plugin->state() == Plugin::State::active);
    CHECK(plugin->latency() == 10);
    CHECK(plugin->tail() == 20);

    // Longer than the plugin was prepared for, so given to it in pieces.
    CHECK(through(*plugin, 2, 300) == std::vector<float>{0.5f, 1.0f});

    auto monoPlugin = rig.made("mono");
    REQUIRE(monoPlugin->prepare({48000.0, 64, false}));
    CHECK(through(*monoPlugin, 2, 100) == std::vector<float>{0.5f, 0.5f});

    plugin->release();
    CHECK(plugin->state() == Plugin::State::inactive);
    CHECK(through(*plugin, 2, 100) == std::vector<float>{1.0f, 2.0f});
    rig.checkNoMisuse();
}

TEST_CASE("Calls on a thread other than the main thread are refused and said")
{
    Rig rig;
    auto plugin = rig.made("plugin");
    const auto before = rig.fake.calls("plugin").size();

    std::thread([&plugin] {
        CHECK_FALSE(plugin->prepare({48000.0, 64, false}));
        CHECK_FALSE(plugin->saveState(PluginStateUse::document).has_value());
        CHECK_FALSE(plugin->loadState("x", PluginStateUse::document));
        CHECK_FALSE(plugin->reset());
        plugin->release();
    }).join();

    CHECK(rig.wrong.size() == 5);
    CHECK(rig.fake.calls("plugin").size() == before);
    CHECK(plugin->state() == Plugin::State::inactive);
}

TEST_CASE("A program can ask a plugin for a layout while it isn't prepared, and it is prepared with what it settles on")
{
    Rig rig;
    FakePluginFormat::Script both;
    both.takes = {{2, 2}, {1, 1}};
    rig.fake.script("both", both);
    FakePluginFormat::Script stereo;
    stereo.takes = {{2, 2}};
    rig.fake.script("only stereo", stereo);

    SECTION("one it takes, given back, kept and played with")
    {
        auto plugin = rig.made("both");
        REQUIRE(plugin->layout() == PluginLayout{2, 2});
        CHECK(plugin->arrange({1, 1}) == PluginLayout{1, 1});
        CHECK(plugin->layout() == PluginLayout{1, 1});
        REQUIRE(plugin->prepare({48000.0, 64, false}));
        CHECK(plugin->layout() == PluginLayout{1, 1});
        CHECK(rig.wrong.empty());
    }

    SECTION("back again")
    {
        auto plugin = rig.made("both");
        CHECK(plugin->arrange({1, 1}) == PluginLayout{1, 1});
        CHECK(plugin->arrange({2, 2}) == PluginLayout{2, 2});
        CHECK(plugin->layout() == PluginLayout{2, 2});
    }

    SECTION("one it won't take gives what it would have settled on, with no harm done")
    {
        auto plugin = rig.made("only stereo");
        CHECK(plugin->arrange({1, 1}) == PluginLayout{2, 2});
        CHECK(plugin->layout() == PluginLayout{2, 2});
        CHECK(plugin->state() == Plugin::State::inactive);
        REQUIRE(plugin->prepare({48000.0, 64, false}));
        CHECK(rig.wrong.empty());
    }

    SECTION("the one asked for is kept when it changes its buses, as one it wouldn't take isn't")
    {
        auto plugin = rig.made("both");
        REQUIRE(plugin->arrange({1, 1}) == PluginLayout{1, 1});
        rig.fake.notify("both", PluginNotice::buses);
        REQUIRE(rig.main.turnUntil([&] { return rig.main.turn() == 0; }));
        CHECK(plugin->layout() == PluginLayout{1, 1});
        REQUIRE(plugin->arrange({2, 2}) == PluginLayout{2, 2});

        auto other = rig.made("only stereo");
        REQUIRE(other->arrange({1, 1}) == PluginLayout{2, 2});
        rig.fake.notify("only stereo", PluginNotice::buses);
        REQUIRE(rig.main.turnUntil([&] { return rig.main.turn() == 0; }));
        CHECK(other->layout() == PluginLayout{2, 2});
    }

    SECTION("refused while prepared, the plugin never asked")
    {
        auto plugin = rig.made("both");
        REQUIRE(plugin->prepare({48000.0, 64, false}));
        CHECK(plugin->arrange({1, 1}) == PluginLayout{2, 2});
        CHECK(plugin->layout() == PluginLayout{2, 2});
        REQUIRE(rig.wrong.size() == 1);
        CHECK(rig.wrong[0].find("arrange") != std::string::npos);
        CHECK(plugin->state() == Plugin::State::active);

        // Once it isn't, it can be.
        plugin->release();
        CHECK(plugin->arrange({1, 1}) == PluginLayout{1, 1});
    }

    SECTION("refused on another thread")
    {
        auto plugin = rig.made("both");
        std::thread([&] { CHECK(plugin->arrange({1, 1}) == PluginLayout{2, 2}); }).join();
        CHECK(plugin->layout() == PluginLayout{2, 2});
        CHECK(rig.wrong.size() == 1);
    }
}

TEST_CASE("A plugin's notices, from any thread and from inside its own calls, are acted on from the main thread")
{
    Rig rig;
    std::atomic<int> activations{0};
    FakePluginFormat::Script noisy;
    noisy.inside = [&rig, &activations](std::string_view call) {
        // Notices from inside the host's own calls into it, which must never be acted on from there.
        if (call == "activate" && ++activations <= 3)
            rig.fake.notify("noisy", PluginNotice::latency | PluginNotice::values);
        else if (call == "setState" || call == "process")
            rig.fake.notify("noisy", PluginNotice::state | PluginNotice::parameters);
    };
    rig.fake.script("noisy", noisy);
    Rig::Heard heard;
    auto plugin = rig.made("noisy", &heard);
    REQUIRE(plugin->prepare({48000.0, 64, false}));

    std::atomic<bool> running{true};
    std::thread own([&] {
        // And from a thread of its own.
        rig.fake.notify("noisy", PluginNotice::latency);

        while (running)
        {
            rig.fake.notify("noisy", PluginNotice::values);
            std::this_thread::sleep_for(1ms);
        }
    });

    {
        AudioThread audio(plugin);
        const auto until = std::chrono::steady_clock::now() + 1s;

        // States given for the first half, the state it says changed after that being told once the 250 ms after
        // the last have passed.
        while (std::chrono::steady_clock::now() < until)
        {
            rig.main.turn();

            if (std::chrono::steady_clock::now() < until - 500ms)
                plugin->loadState("settings", PluginStateUse::document);

            std::this_thread::sleep_for(1ms);
        }

        running = false;
        own.join();
    }

    rig.main.settle();
    rig.checkNoMisuse();
    CHECK(rig.wrong.empty());
    CHECK(heard.has(PluginChange::latency));
    CHECK(heard.has(PluginChange::values));
    CHECK(heard.has(PluginChange::state));
    CHECK(heard.has(PluginChange::parameters));
    CHECK(activations > 1);
    CHECK(plugin->state() == Plugin::State::active);
}

TEST_CASE("While a call into a plugin turns the run loop, calls into it and its taking down wait until it returns")
{
    Rig rig;
    std::shared_ptr<Plugin> plugin;
    std::vector<std::string> happened;
    bool inside = false;
    FakePluginFormat::Script modal;
    modal.inside = [&](std::string_view call) {
        if (call != "setState")
            return;

        // A dialog of the plugin's own: the run loop turns inside its call, running what the program posted.
        inside = true;
        rig.main.post()([&] {
            CHECK(plugin->isBusy());
            CHECK_FALSE(plugin->prepare({44100.0, 32, false}));
            plugin->whenFree(
                [&] { happened.push_back(plugin->prepare({44100.0, 32, false}) ? "prepared" : "refused"); });
            happened.push_back("asked while busy");
        });
        rig.main.turn();
        rig.main.turn();
        happened.push_back("call returning");
        inside = false;
    };
    rig.fake.script("modal", modal);
    plugin = rig.made("modal");

    REQUIRE(plugin->loadState("settings", PluginStateUse::document));
    rig.main.settle();

    CHECK(happened == std::vector<std::string>{"asked while busy", "call returning", "prepared"});
    rig.checkNoMisuse();

    // Let go of while its call turns the run loop: taken down once the call returns, never inside it.
    modal.inside = [&](std::string_view call) {
        if (call == "setState")
        {
            rig.main.post()([&] { plugin.reset(); });
            rig.main.turn();
            rig.main.turn();
            CHECK(rig.fake.alive() == 1);
        }
    };
    rig.fake.script("modal", modal);
    auto second = rig.made("modal");
    plugin = second;
    second.reset();
    REQUIRE(plugin != nullptr);
    plugin->loadState("settings", PluginStateUse::document);
    rig.main.settle();

    CHECK(rig.fake.alive() == 0);
    rig.checkNoMisuse();
}

TEST_CASE("Taking a plugin out of processing waits for the audio thread to leave it, and never overlaps a process call")
{
    Rig rig;
    auto plugin = rig.made("plugin");
    REQUIRE(plugin->prepare({48000.0, 64, false}));
    AudioThread audio(plugin);

    for (int i = 0; i < 200; ++i)
    {
        REQUIRE(plugin->prepare({i % 2 == 0 ? 44100.0 : 48000.0, 64, false}));
        REQUIRE(plugin->reset());
        rig.main.turn();
    }

    CHECK(audio.blocks > 0);
    rig.checkNoMisuse();
}

TEST_CASE("A plugin stuck in its process call is left out after the limit, and the main thread is held no longer")
{
    Rig rig;
    std::timed_mutex held;
    std::atomic<bool> stick{false};
    FakePluginFormat::Script stuck;
    stuck.inside = [&](std::string_view call) {
        if (call == "process" && stick)
        {
            std::unique_lock wait(held);   // held by the test, as a plugin's own lock held by its window
        }
    };
    rig.fake.script("stuck", stuck);

    PluginHost quick{
        {rig.main.post(), rig.main.isCurrent(), 200ms, [&rig](const std::string& what) { rig.wrong.push_back(what); }}};
    auto plugin = quick.make(rig.fake, FakePluginFormat::describe("stuck"));
    Rig::Heard heard;
    plugin->onChange = [&heard](PluginChange change) { heard.changes.push_back(change); };
    REQUIRE(rig.main.turnUntil([&] { return plugin->state() != Plugin::State::making; }));
    REQUIRE(plugin->prepare({48000.0, 64, false}));

    held.lock();
    stick = true;
    {
        AudioThread audio(plugin);
        std::this_thread::sleep_for(50ms);

        const auto started = std::chrono::steady_clock::now();
        CHECK_FALSE(plugin->prepare({44100.0, 64, false}));
        const auto took = std::chrono::steady_clock::now() - started;

        CHECK(took >= 150ms);
        CHECK(took < 1s);
        CHECK(plugin->state() == Plugin::State::failed);
        rig.main.settle();
        CHECK(heard.has(PluginChange::stuck));

        stick = false;
        held.unlock();
        std::this_thread::sleep_for(50ms);
    }

    // Never called again once left out.
    const auto calls = rig.fake.calls("stuck").size();
    through(*plugin, 2, 64);
    CHECK(rig.fake.calls("stuck").size() == calls);
}

TEST_CASE("The last hold on a plugin let go of on another thread takes it down on the main thread, without waiting")
{
    Rig rig;
    auto plugin = rig.made("plugin");
    REQUIRE(plugin->prepare({48000.0, 64, false}));
    through(*plugin, 2, 64);

    // As an export thread finishing with a copy while the main thread joins it.
    std::thread exporter([held = std::move(plugin)]() mutable { held.reset(); });
    const auto started = std::chrono::steady_clock::now();
    exporter.join();
    CHECK(std::chrono::steady_clock::now() - started < 500ms);

    CHECK(rig.fake.alive() == 1);
    rig.main.settle();
    CHECK(rig.fake.alive() == 0);
    CHECK(rig.fake.allTakenApartOnMainThread());
    rig.checkNoMisuse();
}

TEST_CASE("A plugin is taken down in order: processing stopped, reset, deactivated, taken apart")
{
    Rig rig;
    auto plugin = rig.made("plugin");
    REQUIRE(plugin->prepare({48000.0, 64, false}));
    through(*plugin, 2, 64);
    plugin.reset();
    rig.main.settle();

    auto calls = rig.fake.calls("plugin");
    REQUIRE(calls.size() >= 4);
    calls.erase(calls.begin(), calls.end() - 4);
    CHECK(calls == std::vector<std::string>{"stopProcessing", "reset", "deactivate", "destroy"});
    rig.checkNoMisuse();
}

TEST_CASE("What a call into a plugin changes of the whole process is put back")
{
    Rig rig;
    const std::string locale = std::setlocale(LC_ALL, nullptr);
    const auto folder = std::filesystem::current_path();
    const int rounding = std::fegetround();
    std::string otherLocale;

    for (const char* candidate :
         {"de_DE.UTF-8", "de_DE", "fr_FR.UTF-8", "C.UTF-8", "en_US.UTF-8", "German_Germany.1252", ".UTF8"})
    {
        if (const char* set = std::setlocale(LC_ALL, candidate); set != nullptr && locale != set)
        {
            otherLocale = set;
            break;
        }
    }

    std::setlocale(LC_ALL, locale.c_str());
    FakePluginFormat::Script rude;
    rude.inside = [&](std::string_view call) {
        if (call != "activate")
            return;

        if (!otherLocale.empty())
            std::setlocale(LC_ALL, otherLocale.c_str());

        std::fesetround(FE_TOWARDZERO);
        std::filesystem::current_path(std::filesystem::temp_directory_path());
    };
    rig.fake.script("rude", rude);
    Rig::Heard heard;
    auto plugin = rig.made("rude", &heard);

    REQUIRE(plugin->prepare({48000.0, 64, false}));
    rig.main.settle();

    CHECK(std::string(std::setlocale(LC_ALL, nullptr)) == locale);
    CHECK(std::filesystem::current_path() == folder);
    CHECK(std::fegetround() == rounding);
    CHECK(heard.has(PluginChange::processState));
    std::filesystem::current_path(folder);
}

TEST_CASE("A call into a plugin that throws fails the plugin, saying which call, and nothing reaches the program")
{
    Rig rig;
    FakePluginFormat::Script throws;
    throws.inside = [](std::string_view call) {
        if (call == "setState")
            throw std::runtime_error("the plugin's own exception");
    };
    rig.fake.script("throws", throws);
    Rig::Heard heard;
    auto plugin = rig.made("throws", &heard);
    REQUIRE(plugin->prepare({48000.0, 64, false}));

    CHECK_FALSE(plugin->loadState("settings", PluginStateUse::document));
    rig.main.settle();

    CHECK(plugin->state() == Plugin::State::failed);
    CHECK(plugin->problem().message.find("the plugin's own exception") != std::string::npos);
    CHECK(plugin->problem().step == "loadState");
    CHECK(heard.has(PluginChange::failed));

    const auto calls = rig.fake.calls("throws").size();
    CHECK(through(*plugin, 2, 64) == std::vector<float>{1.0f, 2.0f});
    CHECK(rig.fake.calls("throws").size() == calls);

    // Plugins aren't bound to throw only the standard library's exceptions.
    FakePluginFormat::Script throwsAnything;
    throwsAnything.inside = [](std::string_view call) {
        if (call == "setState")
            throw 42;
    };
    rig.fake.script("throws anything", throwsAnything);
    auto other = rig.made("throws anything");

    CHECK_FALSE(other->loadState("settings", PluginStateUse::document));
    CHECK(other->state() == Plugin::State::failed);
    CHECK(other->problem().step == "loadState");
}

TEST_CASE("Calls in the wrong state are refused and said")
{
    Rig rig;
    auto plugin = rig.make("plugin");

    CHECK_FALSE(plugin->prepare({48000.0, 64, false}));
    CHECK_FALSE(plugin->reset());
    CHECK(rig.wrong.size() == 2);

    rig.main.settle();
    REQUIRE(plugin->state() == Plugin::State::inactive);
    CHECK_FALSE(plugin->reset());
    CHECK(rig.wrong.size() == 3);
    CHECK(rig.fake.calls("plugin") == std::vector<std::string>{"make", "arrange", "parameters"});
}

TEST_CASE("A notice a plugin sends from inside its process call reaches the main thread")
{
    Rig rig;
    FakePluginFormat::Script fromAudio;
    fromAudio.inside = [&rig](std::string_view call) {
        if (call == "process")
            rig.fake.notify("from audio", PluginNotice::state);
    };
    rig.fake.script("from audio", fromAudio);
    Rig::Heard heard;
    auto plugin = rig.made("from audio", &heard);
    REQUIRE(plugin->prepare({48000.0, 64, false}));

    AudioThread audio(plugin);
    CHECK(rig.main.turnUntil([&] { return heard.has(PluginChange::state); }, 2s));
}

TEST_CASE("What a plugin changes of the floating-point modes in its process call goes no further than the call")
{
    Rig rig;
    FakePluginFormat::Script rounding;
    rounding.inside = [](std::string_view call) {
        if (call == "process")
            std::fesetround(FE_TOWARDZERO);
    };
    rig.fake.script("rounding", rounding);
    auto plugin = rig.made("rounding");
    REQUIRE(plugin->prepare({48000.0, 64, false}));

    int after = -1;
    std::thread([&] {
        through(*plugin, 2, 64);
        after = std::fegetround();
    }).join();
    CHECK(after == FE_TONEAREST);
}

TEST_CASE("A plugin that can't be prepared says why, and the audio passes it by")
{
    Rig rig;
    FakePluginFormat::Script refuses;
    refuses.failToActivate = true;
    rig.fake.script("refuses", refuses);
    auto plugin = rig.made("refuses");

    CHECK_FALSE(plugin->prepare({48000.0, 64, false}));
    CHECK(plugin->state() == Plugin::State::inactive);
    CHECK(plugin->problem().message == "The fake plugin was told not to activate.");
    CHECK(plugin->problem().step == "prepare");
    CHECK(through(*plugin, 2, 64) == std::vector<float>{1.0f, 2.0f});
    rig.checkNoMisuse();
}

TEST_CASE("A plugin that throws from its processing is switched off, its audio passing by, and nothing else calls it")
{
    Rig rig;
    std::atomic<int> blocks{0};
    FakePluginFormat::Script throws;
    throws.gain = 0.5f;
    throws.inside = [&blocks](std::string_view call) {
        if (call == "process" && ++blocks == 2)
            throw std::runtime_error("the plugin's own exception while processing");
    };
    rig.fake.script("throws", throws);
    Rig::Heard heard;
    auto plugin = rig.made("throws", &heard);
    REQUIRE(plugin->prepare({48000.0, 64, false}));

    CHECK(through(*plugin, 2, 64) == std::vector<float>{0.5f, 1.0f});

    // Split in two, the second half isn't given to it once the first has failed, and nor is the next block, before the
    // main thread has heard.
    CHECK(through(*plugin, 2, 128) == std::vector<float>{1.0f, 2.0f});
    CHECK(through(*plugin, 2, 64) == std::vector<float>{1.0f, 2.0f});
    CHECK(blocks == 2);
    REQUIRE(rig.main.turnUntil([&] { return heard.has(PluginChange::failed); }));
    rig.main.settle();

    CHECK(plugin->state() == Plugin::State::failed);
    CHECK(plugin->problem().step == "process");
    CHECK_FALSE(heard.has(PluginChange::crashed));

    const auto calls = rig.fake.calls("throws").size();
    CHECK(through(*plugin, 2, 64) == std::vector<float>{1.0f, 2.0f});
    CHECK(blocks == 2);
    CHECK(rig.fake.calls("throws").size() == calls);
}

TEST_CASE("A block that comes while a plugin is held for a call that can't overlap its processing is silent, and one "
          "that comes once it has failed passes as it came")
{
    Rig rig;

    // Plays one block of ones through the plugin on an audio thread of the test's own once told to.
    const auto playOnes = [](Plugin& plugin) {
        std::vector<float> left(64, 1.0f), right(64, 1.0f);
        float* channels[] = {left.data(), right.data()};
        plugin.process(channels, 2, 64);
        return left;
    };

    // A reset that takes its time, during which a block comes.
    FakePluginFormat::Script slow;
    slow.gain = 0.5f;
    std::atomic<bool> resetting{false};
    slow.inside = [&resetting](std::string_view call) {
        if (call == "reset")
        {
            resetting = true;
            std::this_thread::sleep_for(300ms);
            resetting = false;
        }
    };
    rig.fake.script("slowReset", slow);
    auto plugin = rig.made("slowReset");
    REQUIRE(plugin->prepare({48000.0, 256, false}));
    std::vector<float> during;
    std::thread audio([&] {
        while (!resetting)
            std::this_thread::sleep_for(1ms);
        during = playOnes(*plugin);
    });
    REQUIRE(plugin->reset());
    audio.join();
    CHECK(std::all_of(during.begin(), during.end(), [](float sample) { return sample == 0.0f; }));
    std::vector<float> after;
    std::thread([&] { after = playOnes(*plugin); }).join();
    CHECK(after[0] == 0.5f);

    // A reset that fails it, after which the audio passes it by.
    FakePluginFormat::Script throwing;
    throwing.gain = 0.5f;
    throwing.inside = [](std::string_view call) {
        if (call == "reset")
            throw std::runtime_error("The reset failed.");
    };
    rig.fake.script("throwingReset", throwing);
    auto broken = rig.made("throwingReset");
    REQUIRE(broken->prepare({48000.0, 256, false}));
    CHECK_FALSE(broken->reset());
    CHECK(broken->state() == Plugin::State::failed);
    std::thread([&] { after = playOnes(*broken); }).join();
    CHECK(after[0] == 1.0f);
}

TEST_CASE("A hold that leaves a plugin off the audio, as when its state is taken unprepared or it can't be prepared "
          "again, by the program or after a notice, lets the audio pass it as it came again")
{
    Rig rig;
    const auto playOnes = [](Plugin& plugin) {
        std::vector<float> left(64, 1.0f), right(64, 1.0f);
        std::thread([&] {
            float* channels[] = {left.data(), right.data()};
            plugin.process(channels, 2, 64);
        }).join();
        return left[0];
    };
    FakePluginFormat::Script once;
    once.gain = 0.5f;
    once.activations = 1;
    PluginParameter level;
    level.id = 7;
    level.name = "Level";
    once.parameters = {level};
    rig.fake.script("once", once);

    // Prepared to hear the value before its state is taken, then for a device, then refusing.
    auto twice = once;
    twice.activations = 2;
    rig.fake.script("twice", twice);
    auto plugin = rig.made("twice");

    // Its state taken while it isn't prepared, with a value waiting for it.
    REQUIRE(plugin->setParameter(7, 0.25));
    REQUIRE(plugin->saveState(PluginStateUse::document));
    CHECK(playOnes(*plugin) == 1.0f);

    // Prepared, then refusing to be prepared again.
    REQUIRE(plugin->prepare({48000.0, 256, false}));
    CHECK(playOnes(*plugin) == 0.5f);
    CHECK_FALSE(plugin->prepare({48000.0, 512, false}));
    CHECK(plugin->state() == Plugin::State::inactive);
    CHECK(playOnes(*plugin) == 1.0f);

    // Prepared, then saying its latency changed, and refusing to be prepared again for it.
    auto noticed = rig.made("once");
    REQUIRE(noticed->prepare({48000.0, 256, false}));
    rig.fake.notify("once", PluginNotice::latency);
    REQUIRE(rig.main.turnUntil([&] { return noticed->state() != Plugin::State::active; }, 2s));
    CHECK(noticed->state() == Plugin::State::inactive);
    CHECK(playOnes(*noticed) == 1.0f);
    rig.checkNoMisuse();
}

TEST_CASE("A reset a plugin asks for from inside its processing comes from the main thread at once, never waiting "
          "for the host's next look at what plugins said there",
          "[timing]")
{
    Rig rig;
    const auto now = [] { return std::chrono::steady_clock::now(); };
    std::atomic<bool> ask{false};
    std::atomic<bool> reset{false};
    FakePluginFormat::Script asking;
    asking.inside = [&](std::string_view call) {
        if (call == "process" && ask.exchange(false))
            rig.fake.notify("asking", PluginNotice::reset);
        if (call == "reset")
            reset = true;
    };
    rig.fake.script("asking", asking);
    auto plugin = rig.made("asking");
    REQUIRE(plugin->prepare({48000.0, 64, false}));

    // The host looks every 10 ms, and each reset is asked for just after the last, so waiting for its look takes about
    // 10 ms every time; woken, it takes well under a millisecond, and a few on a slow machine under the sanitizers.
    std::vector<std::chrono::steady_clock::duration> waits;
    for (int i = 0; i < 21; ++i)
    {
        reset = false;
        ask = true;
        const auto asked = now();
        std::thread([&] {
            std::vector<float> left(64, 1.0f), right(64, 1.0f);
            float* channels[] = {left.data(), right.data()};
            plugin->process(channels, 2, 64);
        }).join();
        REQUIRE(rig.main.turnUntil([&] { return reset.load(); }, 2s));
        waits.push_back(now() - asked);
    }
    std::sort(waits.begin(), waits.end());
    CHECK(waits[10] < 6ms);
    rig.checkNoMisuse();
}

namespace
{
// Plays one block of ones through a plugin, each channel as it comes back.
std::vector<std::vector<float>> playedOnes(Plugin& plugin, int frames = 64, float value = 1.0f)
{
    std::vector<std::vector<float>> audio(2, std::vector<float>(static_cast<std::size_t>(frames), value));
    float* channels[] = {audio[0].data(), audio[1].data()};
    plugin.process(channels, 2, frames);
    return audio;
}

bool allAre(const std::vector<std::vector<float>>& audio, float value)
{
    return std::all_of(audio.begin(), audio.end(), [value](const std::vector<float>& channel) {
        return std::all_of(channel.begin(), channel.end(), [value](float sample) { return sample == value; });
    });
}
}

TEST_CASE("A plugin saying its latency changed as it is activated isn't activated again for that", "[timing]")
{
    Rig rig;
    std::atomic<int> activations{0};
    FakePluginFormat::Script noisy;
    noisy.inside = [&](std::string_view call) {
        if (call == "activate")
        {
            ++activations;
            rig.fake.notify("noisy", PluginNotice::latency);
        }
    };
    rig.fake.script("noisy", noisy);
    auto plugin = rig.made("noisy");
    REQUIRE(plugin->prepare({48000.0, 64, false}));

    const auto until = std::chrono::steady_clock::now() + 300ms;
    while (std::chrono::steady_clock::now() < until)
    {
        rig.main.turn();
        std::this_thread::sleep_for(1ms);
    }

    CHECK(activations == 1);
    CHECK(plugin->state() == Plugin::State::active);
    rig.checkNoMisuse();
}

TEST_CASE("A plugin saying over and over that what hasn't changed has is activated again only after longer and longer",
          "[timing]")
{
    for (const auto notice : {PluginNotice::latency, PluginNotice::buses})
    {
        CAPTURE(static_cast<int>(notice));
        Rig rig;
        std::atomic<int> activations{0};
        FakePluginFormat::Script noisy;
        noisy.inside = [&, notice](std::string_view call) {
            if (call == "activate")
                ++activations;
            else if (call == "process")
                rig.fake.notify("noisy", notice);
        };
        rig.fake.script("noisy", noisy);
        auto plugin = rig.made("noisy");
        REQUIRE(plugin->prepare({48000.0, 64, false}));

        {
            AudioThread audio(plugin);
            const auto until = std::chrono::steady_clock::now() + 1500ms;
            while (std::chrono::steady_clock::now() < until)
            {
                rig.main.turn();
                std::this_thread::sleep_for(1ms);
            }
            CHECK(audio.blocks > 0);
        }

        // As it was prepared, for the first notice, and once the second second's wait has passed at most.
        CHECK(activations >= 2);
        CHECK(activations <= 4);
        CHECK(plugin->state() == Plugin::State::active);
        rig.checkNoMisuse();
    }
}

TEST_CASE("A notice that comes while one that changed nothing is waited out is still acted on once the wait is over",
          "[timing]")
{
    Rig rig;
    std::atomic<int> activations{0};
    FakePluginFormat::Script once;
    once.inside = [&](std::string_view call) {
        if (call == "activate")
            ++activations;
    };
    rig.fake.script("once", once);
    auto plugin = rig.made("once");
    REQUIRE(plugin->prepare({48000.0, 64, false}));

    rig.fake.notify("once", PluginNotice::latency);
    REQUIRE(rig.main.turnUntil([&] { return activations == 2; }, 2s));

    // Nothing more comes from the plugin after this, so only the wait ending can bring it about.
    rig.fake.notify("once", PluginNotice::buses);
    rig.main.settle();
    CHECK(activations == 2);
    CHECK(rig.main.turnUntil([&] { return activations == 3; }, 5s));
    CHECK(plugin->state() == Plugin::State::active);
    rig.checkNoMisuse();
}

TEST_CASE("What a stuck plugin can still reach after the program lets it go stays alive")
{
    Rig rig;
    std::timed_mutex held;
    std::atomic<bool> stick{false}, inside{false};
    FakePluginFormat::Script stuck;
    stuck.inside = [&](std::string_view call) {
        if (call == "process" && stick)
        {
            inside = true;
            std::unique_lock wait(held);
        }
    };
    rig.fake.script("stuck", stuck);

    PluginHost quick{
        {rig.main.post(), rig.main.isCurrent(), 100ms, [&rig](const std::string& what) { rig.wrong.push_back(what); }}};
    auto plugin = quick.make(rig.fake, FakePluginFormat::describe("stuck"));
    REQUIRE(rig.main.turnUntil([&] { return plugin->state() != Plugin::State::making; }));
    REQUIRE(plugin->prepare({48000.0, 64, false}));

    held.lock();
    stick = true;

    // The audio thread calling through a plain pointer, as a device's callback does.
    std::thread audio([raw = plugin.get()] { static_cast<void>(playedOnes(*raw)); });
    while (!inside)
        std::this_thread::sleep_for(1ms);

    CHECK_FALSE(plugin->prepare({44100.0, 64, false}));
    CHECK(plugin->state() == Plugin::State::failed);

    // The program lets it go. The plugin's own thread still says things, and its call at last returns.
    plugin.reset();
    rig.fake.notify("stuck", PluginNotice::latency);
    rig.fake.notify("stuck", PluginNotice::values);
    stick = false;
    held.unlock();
    audio.join();
    rig.main.settle();
    rig.fake.notify("stuck", PluginNotice::latency);
    rig.main.settle();
}

TEST_CASE("A block holding a value that isn't a number plays as it came, and the plugin is reset and faded back in")
{
    for (const float bad : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                            -std::numeric_limits<float>::infinity()})
    {
        CAPTURE(bad);
        Rig rig;
        std::atomic<bool> blown{false};
        std::atomic<int> resets{0}, blocks{0};
        FakePluginFormat::Script script;
        script.gain = 0.5f;
        script.afterBlock = [&](const PluginBlock& block) {
            ++blocks;
            if (blown)
                block.outputs[1][3] = bad;
        };
        script.inside = [&](std::string_view call) {
            if (call == "reset")
            {
                ++resets;
                blown = false;
            }
        };
        rig.fake.script("plugin", script);
        Rig::Heard heard;
        auto plugin = rig.made("plugin", &heard);
        REQUIRE(plugin->prepare({48000.0, 64, false}));
        CHECK(allAre(playedOnes(*plugin), 0.5f));
        CHECK(plugin->notFiniteBlocks() == 0);

        // The block it spoiled, and the next before the main thread has seen, as they came, the plugin left alone.
        blown = true;
        CHECK(allAre(playedOnes(*plugin), 1.0f));
        const auto called = blocks.load();
        CHECK(allAre(playedOnes(*plugin), 1.0f));
        CHECK(blocks == called);

        REQUIRE(rig.main.turnUntil([&] { return resets == 1 && heard.has(PluginChange::notFinite); }, 2s));
        CHECK(plugin->notFiniteBlocks() == 1);
        CHECK(plugin->state() == Plugin::State::active);

        // Back from the audio as it came to the plugin's own, over its first block.
        const auto fading = playedOnes(*plugin);
        for (const auto& channel : fading)
        {
            CHECK(channel.front() > 0.9f);
            CHECK(channel.back() < 0.51f);
            CHECK(std::is_sorted(channel.rbegin(), channel.rend()));
        }
        CHECK(allAre(playedOnes(*plugin), 0.5f));
        rig.checkNoMisuse();
    }
}

TEST_CASE("Audio goes on through a plugin's reset for a value that isn't a number, never silent")
{
    Rig rig;
    std::atomic<bool> blown{false};
    std::atomic<int> resets{0};
    FakePluginFormat::Script script;
    script.afterBlock = [&](const PluginBlock& block) {
        if (blown)
            block.outputs[0][0] = std::numeric_limits<float>::quiet_NaN();
    };
    script.inside = [&](std::string_view call) {
        if (call == "reset")
        {
            ++resets;
            blown = false;
            std::this_thread::sleep_for(5ms);
        }
    };
    rig.fake.script("plugin", script);
    auto plugin = rig.made("plugin");
    REQUIRE(plugin->prepare({48000.0, 64, false}));

    std::atomic<bool> running{true}, silent{false}, notANumber{false};
    std::thread audio([&, raw = plugin.get()] {
        while (running)
        {
            const auto played = playedOnes(*raw);
            for (const auto& channel : played)
                for (const auto sample : channel)
                {
                    silent = silent || sample == 0.0f;
                    notANumber = notANumber || !std::isfinite(sample);
                }
            std::this_thread::sleep_for(100us);
        }
    });

    for (int i = 0; i < 2; ++i)
    {
        blown = true;
        const auto wanted = i + 1;
        REQUIRE(rig.main.turnUntil([&] { return resets == wanted; }, 2s));
        rig.main.settle();
        std::this_thread::sleep_for(1100ms);
    }
    running = false;
    audio.join();

    CHECK_FALSE(silent);
    CHECK_FALSE(notANumber);
    CHECK(plugin->state() == Plugin::State::active);
    rig.checkNoMisuse();
}

TEST_CASE("A plugin that goes on giving values that aren't numbers after being reset is switched off")
{
    Rig rig;
    FakePluginFormat::Script script;
    script.gain = 0.5f;
    script.afterBlock = [](const PluginBlock& block) { block.outputs[0][0] = std::numeric_limits<float>::quiet_NaN(); };
    rig.fake.script("plugin", script);
    Rig::Heard heard;
    auto plugin = rig.made("plugin", &heard);
    REQUIRE(plugin->prepare({48000.0, 64, false}));

    const auto until = std::chrono::steady_clock::now() + 2s;
    while (plugin->state() != Plugin::State::failed && std::chrono::steady_clock::now() < until)
    {
        CHECK(allAre(playedOnes(*plugin), 1.0f));
        rig.main.turn();
        std::this_thread::sleep_for(1ms);
    }

    CHECK(plugin->state() == Plugin::State::failed);
    rig.main.settle();
    CHECK(heard.has(PluginChange::failed));
    CHECK(allAre(playedOnes(*plugin), 1.0f));
    rig.checkNoMisuse();
}

TEST_CASE("A value that isn't a number coming in isn't held against the plugin passing it on")
{
    Rig rig;
    std::atomic<int> resets{0};
    FakePluginFormat::Script script;
    script.gain = 0.5f;
    script.inside = [&](std::string_view call) {
        if (call == "reset")
            ++resets;
    };
    rig.fake.script("plugin", script);
    Rig::Heard heard;
    auto plugin = rig.made("plugin", &heard);
    REQUIRE(plugin->prepare({48000.0, 64, false}));

    const auto played = playedOnes(*plugin, 64, std::numeric_limits<float>::quiet_NaN());
    CHECK(std::isnan(played[0][0]));
    std::this_thread::sleep_for(50ms);
    rig.main.settle();
    CHECK(resets == 0);
    CHECK_FALSE(heard.has(PluginChange::notFinite));
    CHECK(plugin->notFiniteBlocks() == 0);
    CHECK(allAre(playedOnes(*plugin), 0.5f));
}
