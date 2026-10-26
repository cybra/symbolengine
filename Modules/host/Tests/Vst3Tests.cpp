#include <symbolengine/core/ChildProcess.h>
#include <symbolengine/core/Files.h>
#include <symbolengine/host/Plugin.h>
#include <symbolengine/host/Vst3Format.h>

#include "TestMainThread.h"
#include "Vst3/TestIds.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

using namespace symbolengine;
using namespace std::chrono_literals;

namespace
{

const std::filesystem::path module = SYMBOLENGINE_TEST_VST3;
const std::filesystem::path misuseLog = SYMBOLENGINE_TEST_VST3_LOG;

// What the test plugins found wrong in the calls made into them, which starts empty for each test.
std::string misuses()
{
    std::error_code missing;
    return files::read(misuseLog, missing);
}

struct Rig
{
    Rig()
    {
        std::error_code ignored;
        std::filesystem::remove(misuseLog, ignored);
#if defined(_WIN32)
        _putenv_s("SYMBOLENGINE_TEST_VST3_LOG", misuseLog.string().c_str());
#else
        setenv("SYMBOLENGINE_TEST_VST3_LOG", misuseLog.c_str(), 1);
#endif
    }

    TestMainThread main;
    std::vector<std::string> wrong;
    Vst3Format format{{"SymbolEngine tests", main.post(), main.isCurrent()}};
    PluginHost host{
        {main.post(), main.isCurrent(), 2000ms, [this](const std::string& what) { wrong.push_back(what); }}};
    std::vector<PluginChange> changes;

    std::shared_ptr<Plugin> made(const char* id, const std::filesystem::path& from = module)
    {
        auto plugin = host.make(format, Vst3Format::describe(from, id));
        plugin->onChange = [this](PluginChange change) { changes.push_back(change); };
        REQUIRE(main.turnUntil([&] { return plugin->state() != Plugin::State::making; }));
        return plugin;
    }

    bool heard(PluginChange change) const { return std::find(changes.begin(), changes.end(), change) != changes.end(); }
};

std::vector<float> through(Plugin& plugin, int frames)
{
    std::vector<float> left(static_cast<std::size_t>(frames), 1.0f), right(static_cast<std::size_t>(frames), 2.0f);
    float* channels[] = {left.data(), right.data()};
    plugin.process(channels, 2, frames);
    return {left.back(), right.back()};
}

}

TEST_CASE("A VST3 plugin is made from its module, prepared and played, and finds nothing wrong in the calls")
{
    Rig rig;
    auto plugin = rig.made(testIds::gain);

    INFO(plugin->problem().message);
    REQUIRE(plugin->state() == Plugin::State::inactive);
    CHECK(plugin->layout() == PluginLayout{2, 2});
    REQUIRE(plugin->prepare({48000.0, 64, false}));
    CHECK(through(*plugin, 300) == std::vector<float>{0.5f, 1.0f});

    plugin->release();
    plugin.reset();
    rig.main.settle();
    CHECK(misuses().empty());
    CHECK(rig.wrong.empty());
}

TEST_CASE("A VST3 plugin that describes its buses only once arranged is arranged first, and plays")
{
    Rig rig;
    auto plugin = rig.made(testIds::arranged);

    CHECK(plugin->layout() == PluginLayout{2, 2});
    REQUIRE(plugin->prepare({48000.0, 64, false}));
    CHECK(through(*plugin, 64) == std::vector<float>{0.5f, 1.0f});
    CHECK(misuses().empty());
}

TEST_CASE("A VST3 plugin that takes only mono runs mono, its output on both sides")
{
    Rig rig;
    auto plugin = rig.made(testIds::mono);

    CHECK(plugin->layout() == PluginLayout{1, 1});
    REQUIRE(plugin->prepare({48000.0, 64, false}));
    CHECK(through(*plugin, 64) == std::vector<float>{0.5f, 0.5f});
    CHECK(misuses().empty());
}

TEST_CASE("A VST3 plugin with no audio out is set aside, saying so")
{
    Rig rig;
    auto plugin = rig.made(testIds::noOutput);

    CHECK(plugin->state() == Plugin::State::failed);
    CHECK(plugin->problem().message == "It gives no audio out.");
}

TEST_CASE("A VST3 plugin's state is kept and given back, a raw state taken as its component's")
{
    Rig rig;
    auto plugin = rig.made(testIds::gain);
    const std::int32_t value = 42;
    REQUIRE(plugin->loadState(std::string_view(reinterpret_cast<const char*>(&value), sizeof value),
                              PluginStateUse::document));
    const auto saved = plugin->saveState(PluginStateUse::document);
    REQUIRE(saved.has_value());
    CHECK(saved->find(std::string(reinterpret_cast<const char*>(&value), sizeof value)) != std::string::npos);

    auto other = rig.made(testIds::gain);
    REQUIRE(other->loadState(*saved, PluginStateUse::document));
    CHECK(other->saveState(PluginStateUse::document) == saved);
    CHECK(misuses().empty());
}

TEST_CASE("A VST3 plugin's latency said from its process call on the audio thread is applied from the main thread")
{
    Rig rig;
    auto plugin = rig.made(testIds::latency);
    REQUIRE(plugin->prepare({48000.0, 64, false}));
    CHECK(plugin->latency() == 256);

    std::atomic<bool> running{true};
    std::thread audio([&] {
        while (running)
        {
            through(*plugin, 64);
            std::this_thread::sleep_for(200us);
        }
    });

    CHECK(rig.main.turnUntil([&] { return plugin->latency() == 512 && rig.heard(PluginChange::latency); }, 5s));
    running = false;
    audio.join();
    CHECK(plugin->state() == Plugin::State::active);
    CHECK(misuses().empty());
}

TEST_CASE("A VST3 plugin that throws from its state call fails, and nothing reaches the program")
{
    Rig rig;
    auto plugin = rig.made(testIds::throwing);
    REQUIRE(plugin->prepare({48000.0, 64, false}));

    CHECK_FALSE(plugin->loadState("anything", PluginStateUse::document));
    CHECK(plugin->state() == Plugin::State::failed);
    CHECK(plugin->problem().message.find("the test plugin's own exception") != std::string::npos);
    CHECK(through(*plugin, 64) == std::vector<float>{1.0f, 2.0f});
}

TEST_CASE("A VST3 module's plugins are made and let go of many times over, the module loaded once")
{
    Rig rig;

    for (int i = 0; i < 20; ++i)
    {
        auto plugin = rig.made(testIds::gain);
        REQUIRE(plugin->prepare({44100.0 + i, 32, i % 2 == 1}));
        through(*plugin, 32);
        plugin.reset();
        rig.main.settle();
    }

    CHECK(misuses().empty());
}

TEST_CASE("A VST3 plugin whose identity isn't a class ID, or whose module isn't there, fails saying why")
{
    Rig rig;
    auto wrongId = rig.made("not a class id");
    CHECK(wrongId->state() == Plugin::State::failed);
    CHECK(wrongId->problem());

    auto missing = rig.made(testIds::gain, module.parent_path() / "No Such Plugin.vst3");
    CHECK(missing->state() == Plugin::State::failed);
    CHECK(missing->problem().step == "load");
}

TEST_CASE("A VST3 module is entered once, however many of its plugins are made, and exited once, as the program ends")
{
    const auto lifeLog = misuseLog.parent_path() / "test-vst3-life.txt";
    std::error_code ignored;
    std::filesystem::remove(lifeLog, ignored);

    ChildProcess::Options options;
    options.arguments = {module.string()};
    options.environment = {{"SYMBOLENGINE_TEST_VST3_LIFE", lifeLog.string()}};
    std::error_code error;
    auto program = ChildProcess::start(SYMBOLENGINE_MODULE_LIFE, options, error);
    REQUIRE(program != nullptr);
    const auto ending = program->wait(30s);

    REQUIRE(ending.has_value());
    CHECK(ending->kind == ProcessEnding::Kind::exited);
    CHECK(ending->code == 0);
    CHECK(misuses().empty());
    CHECK(files::read(lifeLog, ignored) == "entered\nexited\n");
}
