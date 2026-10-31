#include <symbolengine/core/ChildProcess.h>
#include <symbolengine/core/Files.h>
#include <symbolengine/host/Plugin.h>
#include <symbolengine/host/Vst3Format.h>

#include "Allocations.h"
#include "Scratch.h"
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

TEST_CASE("A VST3 plugin asked for mono is played through its own one channel, and a mono one asked for stereo isn't")
{
    Rig rig;
    auto plugin = rig.made(testIds::gain);
    REQUIRE(plugin->arrange({1, 1}) == PluginLayout{1, 1});
    REQUIRE(plugin->prepare({48000.0, 64, false}));

    // One channel of ones through a plugin running at half: its output, from one channel in, is one channel out.
    std::vector<float> only(64, 1.0f);
    float* channels[] = {only.data()};
    plugin->process(channels, 1, 64);
    CHECK(only.back() == 0.5f);
    plugin->release();

    // And back to stereo, each side its own again.
    REQUIRE(plugin->arrange({2, 2}) == PluginLayout{2, 2});
    REQUIRE(plugin->prepare({48000.0, 64, false}));
    CHECK(through(*plugin, 64) == std::vector<float>{0.5f, 1.0f});
    plugin->release();

    auto mono = rig.made(testIds::mono);
    CHECK(mono->arrange({2, 2}) == PluginLayout{1, 1});
    CHECK(mono->state() == Plugin::State::inactive);
    CHECK(misuses().empty());
    CHECK(rig.wrong.empty());
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

TEST_CASE(
    "A message between a VST3 plugin's two parts reaches it as the host's own calls do: an exception from it, or "
    "on Windows a crash, fails the plugin without reaching the program, and none reaches a plugin that has failed")
{
    Rig rig;

    for (const double how : {0.5, 1.0})
    {
#if !defined(_WIN32)
        // A crash ends any program but on Windows, where the host catches it.
        if (how == 1.0)
            continue;
#endif
        INFO(how);
        rig.changes.clear();
        auto plugin = rig.made(testIds::gain);
        REQUIRE(plugin->prepare({48000.0, 64, false}));
        REQUIRE(plugin->setParameter(testIds::messageParameter, how));

        // Played on an audio thread, its processor sends its controller two messages, which the host hands over on
        // the main thread.
        std::thread([&] { through(*plugin, 64); }).join();
        CHECK(rig.main.turnUntil([&] { return plugin->state() == Plugin::State::failed; }));
        rig.main.settle();
        CHECK(rig.heard(how == 1.0 ? PluginChange::crashed : PluginChange::failed));
        CHECK(plugin->problem().step == "notify");
        if (how == 0.5)
            CHECK(plugin->problem().message.find("the controller's own exception") != std::string::npos);
        CHECK(through(*plugin, 64) == std::vector<float>{1.0f, 2.0f});
    }

    CHECK(misuses().empty());
}

TEST_CASE("A VST3 plugin's parts are taken apart with nothing of the host's holding them as they end")
{
    Rig rig;
    {
        auto plugin = rig.made(testIds::gain);
        REQUIRE(plugin->prepare({48000.0, 64, false}));
        std::thread([&] { through(*plugin, 64); }).join();
        plugin->release();
    }
    rig.main.settle();
    CHECK(misuses().empty());
}

TEST_CASE("A VST3 plugin takes a state put together from its two parts' own states, as another program kept them")
{
    Rig rig;
    const std::int32_t value = 42;
    const std::string component(reinterpret_cast<const char*>(&value), sizeof value), controller;

    auto plugin = rig.made(testIds::gain);
    REQUIRE(plugin->loadState(Vst3Format::packedState(&component, &controller), PluginStateUse::document));

    // The form it gives its own state in, which holds what its component was given.
    const auto saved = plugin->saveState(PluginStateUse::document);
    REQUIRE(saved);
    CHECK(saved->find(component) != std::string::npos);
    CHECK(saved->starts_with(Vst3Format::packedState(nullptr, nullptr).substr(0, 4)));

    // With only its component's, which is all a plugin of one object keeps.
    auto other = rig.made(testIds::gain);
    REQUIRE(other->loadState(Vst3Format::packedState(&component, nullptr), PluginStateUse::document));
    CHECK(other->saveState(PluginStateUse::document) == saved);
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
    CHECK(files::read(lifeLog, ignored) == "entered\nexited\ntaken down\n");
}

TEST_CASE("Each of a program's VST3 modules exits as it ends, before its own static objects are taken down")
{
    ScratchFolder folder;
    std::vector<std::string> modules{module.string()};
    for (const auto* name : {"Second", "Third"})
    {
        const auto copy = folder.path / name / module.filename();
        std::filesystem::create_directories(copy.parent_path());
        std::filesystem::copy(module, copy, std::filesystem::copy_options::recursive);
        modules.push_back(copy.string());
    }

    const auto lifeLog = folder.path / "life.txt";
    ChildProcess::Options options;
    options.arguments = modules;
    options.environment = {{"SYMBOLENGINE_TEST_VST3_LIFE", lifeLog.string()}};
    std::error_code error;
    auto program = ChildProcess::start(SYMBOLENGINE_MODULE_LIFE, options, error);
    REQUIRE(program != nullptr);
    const auto ending = program->wait(30s);

    REQUIRE(ending.has_value());
    CHECK(ending->kind == ProcessEnding::Kind::exited);
    CHECK(ending->code == 0);
    std::error_code ignored;
#if defined(_WIN32)
    // A library's static objects are taken down as it is detached, once the program's end has run everything else.
    CHECK(files::read(lifeLog, ignored) ==
          "entered\nentered\nentered\nexited\nexited\nexited\ntaken down\ntaken down\ntaken down\n");
#else
    CHECK(files::read(lifeLog, ignored) ==
          "entered\nentered\nentered\nexited\ntaken down\nexited\ntaken down\nexited\ntaken down\n");
#endif
}

TEST_CASE("A VST3 plugin's parameters are listed in its order, known by their IDs, with their names, steps and flags")
{
    Rig rig;
    auto plugin = rig.made(testIds::gain);
    const auto parameters = plugin->parameters();

    REQUIRE(parameters.size() == 10);
    CHECK(parameters[0].id == testIds::gainParameter);
    CHECK(parameters[0].name == "Gain");
    CHECK(parameters[0].units == "x");
    CHECK(parameters[0].defaultValue == 0.5);
    CHECK(parameters[0].automatable);
    CHECK(parameters[1].hidden);
    CHECK(parameters[2].readOnly);
    CHECK(parameters[3].bypass);
    CHECK(parameters[3].steps == 1);
    CHECK(parameters[4].programChange);
    CHECK(parameters[4].steps == 3);
    CHECK(plugin->parameter(testIds::gainParameter) == 0.5);
    CHECK_FALSE(plugin->parameterText(testIds::gainParameter, 0.25).empty());
    CHECK_FALSE(plugin->setParameter(testIds::meterParameter, 0.5));
}

TEST_CASE("A value set with no device running reaches a VST3 plugin's processing before its state is taken, "
          "one that passes over a call with no samples included")
{
    Rig rig;

    for (const char* id : {testIds::gain, testIds::skipsEmpty})
    {
        INFO(id);
        auto plugin = rig.made(id);
        REQUIRE(plugin->prepare({48000.0, 64, false}));
        REQUIRE(plugin->setParameter(testIds::gainParameter, 0.25));

        const auto state = plugin->saveState(PluginStateUse::document);
        REQUIRE(state.has_value());
        const float quarter = 0.25f;
        CHECK(state->find(std::string(reinterpret_cast<const char*>(&quarter), sizeof quarter)) != std::string::npos);
        CHECK(through(*plugin, 64) == std::vector<float>{0.25f, 0.5f});
    }

    CHECK(misuses().empty());
}

TEST_CASE("A value set while a device plays reaches a VST3 plugin within a block")
{
    Rig rig;
    auto plugin = rig.made(testIds::gain);
    REQUIRE(plugin->prepare({48000.0, 64, false}));
    std::atomic<bool> running{true};
    std::atomic<float> heard{0.0f};
    std::thread audio([&] {
        while (running)
        {
            heard = through(*plugin, 64).front();
            std::this_thread::sleep_for(200us);
        }
    });

    REQUIRE(rig.main.turnUntil([&] { return heard == 0.5f; }, 2s));
    REQUIRE(plugin->setParameter(testIds::gainParameter, 0.125));
    const bool playedIt = rig.main.turnUntil([&] { return heard == 0.125f; }, 2s);
    running = false;
    audio.join();

    CHECK(playedIt);
    CHECK(misuses().empty());
}

namespace
{

using Told = std::vector<std::pair<PluginEdit, bool>>;
constexpr auto begin = PluginEdit::Kind::begin, value = PluginEdit::Kind::value, end = PluginEdit::Kind::end,
               reported = PluginEdit::Kind::reported;

}

TEST_CASE("A VST3 plugin's own gesture reaches its processor and the program, its controller is shown the value it "
          "left as it was, and its edit of a parameter it never listed is passed over")
{
    Rig rig;
    auto plugin = rig.made(testIds::gain);
    Told told;
    plugin->onEdit = [&told](const PluginEdit& edit, bool byUser) { told.emplace_back(edit, byUser); };
    REQUIRE(plugin->prepare({48000.0, 64, false}));

    REQUIRE(plugin->setParameter(testIds::editParameter, 0.75));
    REQUIRE(rig.main.turnUntil([&] { return told.size() >= 3; }));
    rig.main.settle();

    CHECK(told == Told{{{begin, testIds::gainParameter, 0.0}, true},
                       {{value, testIds::gainParameter, 0.75}, true},
                       {{end, testIds::gainParameter, 0.0}, true}});
    CHECK(plugin->parameter(testIds::gainParameter) == 0.75);
    CHECK(through(*plugin, 64) == std::vector<float>{0.75f, 1.5f});
    CHECK(plugin->parameterText(testIds::queryParameter, 0.0).find("gain=0.750000") != std::string::npos);
    CHECK(misuses().empty());
}

TEST_CASE("A VST3 controller that edits back the value it is given is given it once, with no loop, rounded or not")
{
    Rig rig;
    auto plugin = rig.made(testIds::gain);
    Told told;
    plugin->onEdit = [&told](const PluginEdit& edit, bool byUser) { told.emplace_back(edit, byUser); };

    REQUIRE(plugin->setParameter(testIds::echoParameter, 0.3));
    REQUIRE(plugin->setParameter(testIds::roundedEchoParameter, 0.3));

    for (int i = 0; i < 20; ++i)
        rig.main.settle();

    const auto text = plugin->parameterText(testIds::queryParameter, 0.0);
    INFO(text);
    CHECK(text.find("echo=1 rounded=1") != std::string::npos);
    CHECK(told.empty());
    CHECK(misuses().empty());
}

TEST_CASE("A VST3 processor's reported values reach its controller and the program, never as the user's")
{
    Rig rig;
    auto plugin = rig.made(testIds::gain);
    Told told;
    plugin->onEdit = [&told](const PluginEdit& edit, bool byUser) { told.emplace_back(edit, byUser); };
    REQUIRE(plugin->prepare({48000.0, 64, false}));

    through(*plugin, 64);
    REQUIRE(rig.main.turnUntil([&] { return !told.empty(); }, 2s));
    rig.main.settle();

    CHECK(told.front() == std::make_pair(PluginEdit{reported, testIds::meterParameter, 1.0}, false));
    CHECK(plugin->parameter(testIds::meterParameter) == 1.0);
    CHECK(plugin->parameterText(testIds::queryParameter, 0.0).find("meter=1.000000") != std::string::npos);
    CHECK(misuses().empty());
}

TEST_CASE("A VST3 plugin saying its state changed a hundred times at once is told once")
{
    Rig rig;
    auto plugin = rig.made(testIds::gain);

    REQUIRE(plugin->setParameter(testIds::dirtyParameter, 1.0));
    rig.main.settle();

    CHECK(std::count(rig.changes.begin(), rig.changes.end(), PluginChange::state) == 1);
    CHECK(misuses().empty());
}

TEST_CASE("The program's automation reaches a VST3 processor at its frames, in a block split for it")
{
    Rig rig;
    auto plugin = rig.made(testIds::gain);
    REQUIRE(plugin->prepare({48000.0, 32, false}));
    std::vector<float> left(64, 1.0f), right(64, 2.0f);
    float* channels[] = {left.data(), right.data()};
    const std::vector<PluginParameterChange> changes{{testIds::gainParameter, 16, 0.25},
                                                     {testIds::gainParameter, 40, 1.0}};

    plugin->process(channels, 2, 64, changes.data(), static_cast<int>(changes.size()));

    CHECK(left[15] == 0.5f);
    CHECK(left[16] == 0.25f);
    CHECK(left[39] == 0.25f);
    CHECK(left[40] == 1.0f);
    CHECK(right[63] == 2.0f);
    CHECK(misuses().empty());
}

TEST_CASE("A thousand parameters automated every block, some many times, reach a VST3 plugin with nothing allocated "
          "on the audio thread")
{
    Rig rig;
    auto plugin = rig.made(testIds::many);
    REQUIRE(plugin->parameters().size() == testIds::manyParameters);
    REQUIRE(plugin->prepare({48000.0, 64, false}));

    std::vector<PluginParameterChange> changes;

    for (unsigned id = 1; id <= testIds::manyParameters; ++id)
        changes.push_back({id, static_cast<int>(id % 64), (id % 100) / 100.0});

    for (unsigned id = 1; id <= 50; ++id)
        for (int point = 0; point < 8; ++point)
            changes.push_back({id, point * 8 + 1, point / 8.0});

    std::stable_sort(
        changes.begin(), changes.end(),
        [](const PluginParameterChange& a, const PluginParameterChange& b) { return a.offset < b.offset; });
    std::vector<float> left(64, 1.0f), right(64, 2.0f);
    float* channels[] = {left.data(), right.data()};
    std::uint64_t counted = 0;

    std::thread([&] {
        const auto before = allocations::count();
        {
            const allocations::Counted counting;

            for (int i = 0; i < 100; ++i)
                plugin->process(channels, 2, 64, changes.data(), static_cast<int>(changes.size()));
        }
        counted = allocations::count() - before;
    }).join();

    CHECK(counted == 0);
    CHECK(misuses().empty());
}

TEST_CASE("A VST3 plugin is asked what context it needs before it is activated, and is given it in every call, 120 "
          "beats a minute in 4/4 where the program has no tempo")
{
    Rig rig;
    auto plugin = rig.made(testIds::timed);
    REQUIRE(plugin->prepare({48000.0, 64, false}));

    for (int i = 0; i < 3; ++i)
        through(*plugin, 64);

    const auto text = plugin->parameterText(testIds::queryParameter, 0.0);
    INFO(text);
    CHECK(text.find("position=128 playing=0 tempo=120.0 signature=4/4 music=0.005 bar=0.000 stops=0") !=
          std::string::npos);
    CHECK(misuses().empty());
}

TEST_CASE("The program's time reaches a VST3 plugin at each split block's own first frame, its music position "
          "following its tempo and meter")
{
    Rig rig;
    auto plugin = rig.made(testIds::timed);
    REQUIRE(plugin->prepare({48000.0, 32, false}));
    std::vector<float> left(64, 1.0f), right(64, 2.0f);
    float* channels[] = {left.data(), right.data()};
    const PluginTime time{true, false, false, 96000, 0, 90.0, 3, 4};

    plugin->process(channels, 2, 64, nullptr, 0, &time);

    const auto text = plugin->parameterText(testIds::queryParameter, 0.0);
    INFO(text);
    CHECK(text.find("position=96032 playing=1 tempo=90.0 signature=3/4 music=3.001 bar=3.000") != std::string::npos);
    CHECK(misuses().empty());
}

TEST_CASE("A discontinuity in the program's time stops and starts a VST3 plugin's processing before the block, its "
          "context's counts still running on")
{
    Rig rig;
    auto plugin = rig.made(testIds::timed);
    REQUIRE(plugin->prepare({48000.0, 64, false}));
    std::vector<float> left(64, 1.0f), right(64, 2.0f);
    float* channels[] = {left.data(), right.data()};
    PluginTime time{true, false, false, 0, 0, 0.0, 0, 0};
    plugin->process(channels, 2, 64, nullptr, 0, &time);

    time.position = 480000;
    time.discontinuity = true;
    plugin->process(channels, 2, 64, nullptr, 0, &time);

    const auto text = plugin->parameterText(testIds::queryParameter, 0.0);
    INFO(text);
    CHECK(text.find("position=480000 playing=1") != std::string::npos);
    CHECK(text.find("stops=1") != std::string::npos);
    CHECK(plugin->timing().longestResetWait == 0);
    CHECK(misuses().empty());
}

TEST_CASE("A VST3 plugin prepared again at another rate is given a context at that rate, its counts running on")
{
    Rig rig;
    auto plugin = rig.made(testIds::gain);
    REQUIRE(plugin->prepare({48000.0, 64, false}));
    through(*plugin, 64);
    REQUIRE(plugin->prepare({44100.0, 64, false}));
    through(*plugin, 64);

    CHECK(misuses().empty());
}

TEST_CASE("A VST3 plugin's system time rises in every call, though the program's time and the host's own clock mix")
{
    Rig rig;
    auto plugin = rig.made(testIds::gain);
    REQUIRE(plugin->prepare({48000.0, 64, false}));
    std::vector<float> left(64, 1.0f), right(64, 2.0f);
    float* channels[] = {left.data(), right.data()};
    PluginTime time{true, false, false, 0, 1000, 0.0, 0, 0};
    plugin->process(channels, 2, 64, nullptr, 0, &time);

    // Its state taken with a value waiting and no device playing, handed over on the main thread by the host's clock.
    REQUIRE(plugin->setParameter(testIds::gainParameter, 0.25));
    REQUIRE(plugin->saveState(PluginStateUse::document).has_value());

    time.position = 64;
    time.systemNanoseconds = 2000;
    plugin->process(channels, 2, 64, nullptr, 0, &time);

    CHECK(misuses().empty());
}

TEST_CASE(
    "A VST3 plugin that throws from its processing is switched off, its audio passing by, and the program goes on")
{
    Rig rig;
    auto plugin = rig.made(testIds::faulting);
    REQUIRE(plugin->prepare({48000.0, 64, false}));
    CHECK(through(*plugin, 64) == std::vector<float>{0.25f, 0.5f});

    REQUIRE(plugin->setParameter(testIds::throwInProcess, 1.0));
    CHECK(through(*plugin, 64) == std::vector<float>{1.0f, 2.0f});
    REQUIRE(rig.main.turnUntil([&] { return rig.heard(PluginChange::failed); }));
    rig.main.settle();

    CHECK(plugin->state() == Plugin::State::failed);
    CHECK(plugin->problem().step == "process");
    CHECK_FALSE(rig.heard(PluginChange::crashed));
    CHECK(through(*plugin, 64) == std::vector<float>{1.0f, 2.0f});
}

TEST_CASE(
    "A VST3 plugin that sleeps in every hundredth block has those blocks, and only those, counted late, its worst "
    "block one of them",
    "[timing]")
{
    Rig rig;
    auto plugin = rig.made(testIds::faulting);
    REQUIRE(plugin->prepare({48000.0, 1024, false}));
    REQUIRE(plugin->setParameter(testIds::slowInProcess, 1.0));

    for (int i = 0; i < 250; ++i)
        through(*plugin, 1024);

    // Its 50 ms against a block of 21.3 ms.
    const auto timing = plugin->timing();
    CHECK(timing.blocks == 250);
    CHECK(timing.lateBlocks == 2);
    CHECK(timing.worstNanoseconds >= 50'000'000);
    CHECK(timing.worstShare >= 50.0 / 21.34);
    CHECK(misuses().empty());
}

#if defined(_MSC_VER)

TEST_CASE("A VST3 plugin that crashes in its processing on Windows is switched off and never called again, and the "
          "program is told it crashed")
{
    Rig rig;
    auto plugin = rig.made(testIds::faulting);
    REQUIRE(plugin->prepare({48000.0, 64, false}));

    REQUIRE(plugin->setParameter(testIds::faultInProcess, 1.0));
    CHECK(through(*plugin, 64) == std::vector<float>{1.0f, 2.0f});
    REQUIRE(rig.main.turnUntil([&] { return rig.heard(PluginChange::crashed); }));
    rig.main.settle();

    CHECK(plugin->state() == Plugin::State::failed);
    CHECK(plugin->problem().step == "process");
    CHECK(plugin->problem().code != 0);
    CHECK(through(*plugin, 64) == std::vector<float>{1.0f, 2.0f});

    // Taken down without a call into it.
    plugin.reset();
    rig.main.settle();
}

TEST_CASE("A VST3 plugin that crashes on the main thread on Windows is switched off, and the program is told which "
          "call")
{
    Rig rig;
    auto plugin = rig.made(testIds::faulting);
    REQUIRE(plugin->prepare({48000.0, 64, false}));

    CHECK_FALSE(plugin->setParameter(testIds::faultOnMain, 1.0));
    rig.main.settle();

    CHECK(rig.heard(PluginChange::crashed));
    CHECK(plugin->state() == Plugin::State::failed);
    CHECK(plugin->problem().step == "setParameter");
    CHECK(through(*plugin, 64) == std::vector<float>{1.0f, 2.0f});
    plugin.reset();
    rig.main.settle();
}

#endif

TEST_CASE("A VST3 module's plugins are listed as each declares itself, without making any")
{
    Rig rig;
    PluginError error;
    const auto plugins = rig.format.pluginsIn(module, error);
    CHECK_FALSE(error);

    // Its effect classes in the factory's order, never its controllers.
    std::vector<std::string> ids;
    for (const auto& plugin : plugins)
        ids.push_back(plugin.identity.id);
    CHECK(ids == std::vector<std::string>{testIds::gain, testIds::arranged, testIds::mono, testIds::noOutput,
                                          testIds::latency, testIds::many, testIds::faulting, testIds::timed,
                                          testIds::skipsEmpty, testIds::throwing, testIds::instrument,
                                          testIds::editor, testIds::unsafe, testIds::unsafeTwin});
    REQUIRE(plugins.size() == ids.size());
    CHECK(plugins[0].identity.format == "VST3");
    CHECK(plugins[0].location == module);
    CHECK(plugins[0].name == "Gain");
    CHECK(plugins[0].maker == "SymbolEngine");
    CHECK(plugins[0].categories == "Fx");
    CHECK(plugins[0].version == "1.0.0");
    CHECK(plugins[0].kind == PluginKind::effect);
    CHECK(plugins[0].sdk.starts_with("VST 3."));
    CHECK(plugins[0].layouts.empty());
    CHECK(plugins[10].name == "Instrument");
    CHECK(plugins[10].version == "2.1.0");
    CHECK(plugins[10].kind == PluginKind::instrument);
    CHECK(misuses().empty());

    // A file that isn't a VST3 module says why.
    const auto none = rig.format.pluginsIn(module.parent_path() / "No such module.vst3", error);
    CHECK(none.empty());
    CHECK(error);
}

TEST_CASE("A value set on a VST3 plugin nothing has prepared is in the state taken of it, heard in another made from it")
{
    Rig rig;
    auto plugin = rig.made(testIds::gain);
    REQUIRE(plugin->setParameter(testIds::gainParameter, 0.25));
    const auto saved = plugin->saveState(PluginStateUse::document);
    REQUIRE(saved.has_value());
    CHECK(plugin->state() == Plugin::State::inactive);

    auto other = rig.made(testIds::gain);
    REQUIRE(other->loadState(*saved, PluginStateUse::document));
    REQUIRE(other->prepare({48000.0, 64, false}));
    CHECK(through(*other, 64) == std::vector<float>{0.25f, 0.5f});

    REQUIRE(plugin->prepare({48000.0, 64, false}));
    CHECK(through(*plugin, 64) == std::vector<float>{0.25f, 0.5f});
    CHECK(misuses().empty());
}
