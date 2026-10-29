#include <symbolengine/host/FakePluginFormat.h>
#include <symbolengine/host/Plugin.h>

#include "TestMainThread.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cfenv>
#include <clocale>
#include <filesystem>
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
