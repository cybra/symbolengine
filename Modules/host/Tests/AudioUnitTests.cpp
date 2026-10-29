#include <symbolengine/core/Files.h>
#include <symbolengine/core/System.h>
#include <symbolengine/host/AudioUnitFormat.h>
#include <symbolengine/host/Plugin.h>

#include "TestMainThread.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

using namespace symbolengine;
using namespace std::chrono_literals;

namespace
{

const std::filesystem::path units = SYMBOLENGINE_TEST_AUDIO_UNITS;
const std::filesystem::path misuseLog = SYMBOLENGINE_TEST_AU_LOG;

OSType code(const char (&four)[5])
{
    return static_cast<OSType>(static_cast<unsigned char>(four[0])) << 24 |
           static_cast<OSType>(static_cast<unsigned char>(four[1])) << 16 |
           static_cast<OSType>(static_cast<unsigned char>(four[2])) << 8 |
           static_cast<OSType>(static_cast<unsigned char>(four[3]));
}

// Registers every unit of the test component with this process only, once, so the tests run them uninstalled.
void registerUnits()
{
    static const bool registered = [] {
        const auto path = units.string();
        const auto url = CFURLCreateFromFileSystemRepresentation(nullptr, reinterpret_cast<const UInt8*>(path.c_str()),
                                                                 static_cast<CFIndex>(path.size()), true);
        const auto bundle = CFBundleCreate(nullptr, url);
        CFRelease(url);
        REQUIRE(bundle != nullptr);
        REQUIRE(CFBundleLoadExecutable(bundle));
        const auto list =
            static_cast<CFArrayRef>(CFDictionaryGetValue(CFBundleGetInfoDictionary(bundle), CFSTR("AudioComponents")));
        REQUIRE(list != nullptr);
        for (CFIndex i = 0; i < CFArrayGetCount(list); ++i)
        {
            const auto entry = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(list, i));
            const auto codeOf = [entry](CFStringRef key) {
                char text[8] = {};
                CFStringGetCString(static_cast<CFStringRef>(CFDictionaryGetValue(entry, key)), text, sizeof text,
                                   kCFStringEncodingASCII);
                return static_cast<OSType>(static_cast<unsigned char>(text[0])) << 24 |
                       static_cast<OSType>(static_cast<unsigned char>(text[1])) << 16 |
                       static_cast<OSType>(static_cast<unsigned char>(text[2])) << 8 |
                       static_cast<OSType>(static_cast<unsigned char>(text[3]));
            };
            AudioComponentDescription description{codeOf(CFSTR("type")), codeOf(CFSTR("subtype")),
                                                  codeOf(CFSTR("manufacturer")), 0, 0};
            const auto factory = reinterpret_cast<AudioComponentFactoryFunction>(CFBundleGetFunctionPointerForName(
                bundle, static_cast<CFStringRef>(CFDictionaryGetValue(entry, CFSTR("factoryFunction")))));
            REQUIRE(factory != nullptr);
            REQUIRE(AudioComponentRegister(&description,
                                           static_cast<CFStringRef>(CFDictionaryGetValue(entry, CFSTR("name"))), 1,
                                           factory) != nullptr);
        }
        return true;
    }();
    static_cast<void>(registered);
}

// What the test units found wrong in the calls made into them, which starts empty for each test.
std::string misuses()
{
    std::error_code missing;
    return files::read(misuseLog, missing);
}

struct Rig
{
    Rig()
    {
        registerUnits();
        std::error_code ignored;
        std::filesystem::remove(misuseLog, ignored);
        setenv("SYMBOLENGINE_TEST_AU_LOG", misuseLog.c_str(), 1);
    }

    TestMainThread main;
    std::vector<std::string> wrong;
    AudioUnitFormat format{{main.post(), main.isCurrent()}};
    PluginHost host{
        {main.post(), main.isCurrent(), 2000ms, [this](const std::string& what) { wrong.push_back(what); }}};

    PluginDescription describe(const char (&subtype)[5])
    {
        PluginError error;
        const auto identity = PluginIdentity::audioUnit(code("aufx"), code(subtype), code("SyTs"));
        for (const auto& plugin : format.pluginsIn(units, error))
            if (plugin.identity == identity)
                return plugin;
        FAIL("no test unit " << subtype);
        return {};
    }

    std::shared_ptr<Plugin> made(const char (&subtype)[5], PluginSetup setup = {48000.0, 512, false})
    {
        auto plugin = host.make(format, describe(subtype));
        REQUIRE(main.turnUntil([&] { return plugin->state() != Plugin::State::making; }));
        INFO(plugin->problem().message);
        REQUIRE(plugin->state() == Plugin::State::inactive);
        REQUIRE(plugin->prepare(setup));
        return plugin;
    }
};

// Plays blocks through a plugin on an audio thread of the test's own: stereo, each sample its own number from the
// first, giving what came out.
std::vector<std::vector<float>> play(Plugin& plugin, int frames, int blocks, float start = 1.0f)
{
    std::vector<std::vector<float>> out(2);
    std::thread([&] {
        float next = start;
        for (int b = 0; b < blocks; ++b)
        {
            std::vector<float> left(static_cast<std::size_t>(frames)), right(static_cast<std::size_t>(frames));
            for (int i = 0; i < frames; ++i)
            {
                left[static_cast<std::size_t>(i)] = next;
                right[static_cast<std::size_t>(i)] = -next;
                next = start == 0.0f ? 0.0f : next + 1.0f;
            }
            float* channels[] = {left.data(), right.data()};
            plugin.process(channels, 2, frames);
            out[0].insert(out[0].end(), left.begin(), left.end());
            out[1].insert(out[1].end(), right.begin(), right.end());
        }
    }).join();
    return out;
}

// What a stretch of numbered samples is, from the first number.
std::vector<float> numbered(std::size_t count, float start = 1.0f, float sign = 1.0f)
{
    std::vector<float> samples(count);
    for (std::size_t i = 0; i < count; ++i)
        samples[i] = sign * (start + static_cast<float>(i));
    return samples;
}

}

TEST_CASE("An Audio Unit's component and units are read from its bundle without loading its code")
{
    Rig rig;
    CHECK(rig.format.name() == "AudioUnit");
    CHECK(rig.format.isPluginFile(units));
    CHECK_FALSE(rig.format.isPluginFile(units.parent_path()));
    REQUIRE(rig.format.folders().size() == 2);
    CHECK(rig.format.folders()[0].path == "/Library/Audio/Plug-Ins/Components");
    CHECK_FALSE(rig.format.folders()[0].user);
    CHECK(rig.format.folders()[1].user);

    PluginError error;
    const auto plugins = rig.format.pluginsIn(units, error);
    CHECK_FALSE(error);
    REQUIRE(plugins.size() == 13);
    CHECK(plugins[0].identity == PluginIdentity::audioUnit(code("aufx"), code("Pass"), code("SyTs")));
    CHECK(plugins[0].location == units);
    CHECK(plugins[0].maker == "SymbolEngine Tests");
    CHECK(plugins[0].name == "Pass");
    CHECK(plugins[0].version == "1.2.3");
    CHECK(plugins[0].kind == PluginKind::effect);
    CHECK(plugins[7].identity == PluginIdentity::audioUnit(code("aufx"), code("Ten "), code("SyTs")));

    const auto facts = rig.format.examine(units, error);
    REQUIRE(facts);
    CHECK(facts->stamp.size > 0);
    CHECK(facts->stamp.version == "1.2.3");
    CHECK(std::find(facts->architectures.begin(), facts->architectures.end(), system::architecture()) !=
          facts->architectures.end());
    CHECK(misuses().empty());
}

TEST_CASE("An Audio Unit is made without blocking the main thread, set up stereo at the program's rate and played")
{
    Rig rig;
    auto plugin = rig.made("Pass");
    CHECK(plugin->layout() == PluginLayout{2, 2});
    CHECK(plugin->latency() == 480);
    const auto out = play(*plugin, 512, 4);
    CHECK(out[0] == numbered(2048));
    CHECK(out[1] == numbered(2048, 1.0f, -1.0f));
    plugin.reset();
    rig.main.settle();
    CHECK(misuses().empty());
    CHECK(rig.wrong.empty());
}

TEST_CASE("A failed render is never heard: the block passes as it came, and an instance the system says is gone is "
          "never called again")
{
    Rig rig;
    auto failing = rig.made("Fail");
    const auto out = play(*failing, 256, 60);
    CHECK(std::find(out[0].begin(), out[0].end(), 0.777f) == out[0].end());
    CHECK(out[0] == numbered(256 * 60));
    CHECK(failing->state() == Plugin::State::active);

    auto dead = rig.made("Dead");
    play(*dead, 256, 150);
    rig.main.turnUntil([&] { return dead->state() == Plugin::State::failed; }, 2s);
    CHECK(dead->state() == Plugin::State::failed);
    CHECK(dead->problem().message == "The system says its instance is gone.");
    rig.main.settle();
    CHECK(misuses().empty());
}

TEST_CASE("A unit's largest block is set before it is initialized, and a longer block reaches one that takes less in "
          "pieces")
{
    Rig rig;
    auto plugin = rig.made("Smal", {48000.0, 2048, false});
    const auto out = play(*plugin, 2048, 3);
    auto halved = numbered(2048 * 3);
    for (auto& sample : halved)
        sample *= 0.5f;
    CHECK(out[0] == halved);
    CHECK(misuses().empty());
}

TEST_CASE("The input a unit pulls is given exactly as it asks, in order, with silence past the block")
{
    Rig rig;
    auto plugin = rig.made("Pull");
    const auto out = play(*plugin, 300, 5);
    CHECK(out[0] == numbered(1500));
    CHECK(out[1] == numbered(1500, 1.0f, -1.0f));
    CHECK(misuses().empty());
}

TEST_CASE("Outputs are what the unit wrote, wherever it put them, and a channel it left unwritten is silent")
{
    Rig rig;
    auto plugin = rig.made("Swap");
    const auto out = play(*plugin, 256, 4);
    CHECK(out[0] == numbered(1024));
    CHECK(out[1] == std::vector<float>(1024, 0.0f));
    CHECK(misuses().empty());
}

TEST_CASE("A unit that says its output is silent is still heard, and a reset clears what it held")
{
    Rig rig;
    auto plugin = rig.made("Echo");

    // Its echo of the first block comes 100 frames on, in the second, silent one.
    auto out = play(*plugin, 64, 1);
    out = play(*plugin, 64, 1, 0.0f);
    CHECK(out[0][100 - 64] == 1.0f);
    CHECK(out[0][100 - 64 + 10] == 11.0f);

    // Reset after a block, nothing of it comes out after.
    play(*plugin, 64, 1);
    REQUIRE(plugin->reset());
    out = play(*plugin, 64, 2, 0.0f);
    CHECK(out[0] == std::vector<float>(128, 0.0f));
    CHECK(misuses().empty());
}

TEST_CASE("A unit that starts with ten channels runs stereo")
{
    Rig rig;
    auto plugin = rig.made("Ten ");
    CHECK(plugin->layout() == PluginLayout{2, 2});
    const auto out = play(*plugin, 128, 2);
    CHECK(out[0] == numbered(256));
    CHECK(misuses().empty());
}

TEST_CASE("Units let go of while they play stop rendering, then are uninitialized and disposed on the main thread")
{
    Rig rig;
    for (int round = 0; round < 30; ++round)
    {
        auto plugin = rig.made("Pass");
        std::atomic<bool> going{true};
        std::thread audio([held = plugin, &going]() mutable {
            std::vector<float> left(128, 1.0f), right(128, 1.0f);
            float* channels[] = {left.data(), right.data()};
            while (going)
                held->process(channels, 2, 128);
            held.reset();
        });
        std::this_thread::sleep_for(5ms);
        plugin.reset();
        going = false;
        audio.join();
        rig.main.settle();
    }
    CHECK(misuses().empty());
    CHECK(rig.wrong.empty());
}

TEST_CASE("A unit that changes its buses as it is initialized is refused, saying so")
{
    Rig rig;
    auto plugin = rig.host.make(rig.format, rig.describe("Grow"));
    REQUIRE(rig.main.turnUntil([&] { return plugin->state() != Plugin::State::making; }));
    REQUIRE(plugin->state() == Plugin::State::inactive);
    CHECK_FALSE(plugin->prepare({48000.0, 512, false}));
    CHECK(plugin->problem().message == "It changed its buses as it was initialized.");
    plugin.reset();
    rig.main.settle();
    CHECK(misuses().empty());
}

// Plays a block through a plugin on an audio thread of the test's own with where the program is in time for it, giving
// what came out.
std::vector<std::vector<float>> playAt(Plugin& plugin, int frames, const PluginTime& time)
{
    std::vector<std::vector<float>> out(2, std::vector<float>(static_cast<std::size_t>(frames), 0.25f));
    std::thread([&] {
        float* channels[] = {out[0].data(), out[1].data()};
        plugin.process(channels, 2, frames, nullptr, 0, &time);
    }).join();
    return out;
}

std::int64_t nanosecondsNow()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

TEST_CASE("The host's callbacks give the program's tempo, meter and transport in a render and nothing outside one, "
          "and time stamps run on")
{
    Rig rig;
    auto plugin = rig.made("Time");

    // With no tempo of the program's, 120 beats a minute.
    auto out = play(*plugin, 256, 4);
    CHECK(out[0][0] == Catch::Approx(0.12f));

    // The program's own tempo and place: a second in at 90 beats a minute is beat 1.5.
    PluginTime time{true, false, false, 48000, nanosecondsNow(), 90.0, 3, 4};
    out = playAt(*plugin, 256, time);
    CHECK(out[0][0] == Catch::Approx(0.09f));
    CHECK(out[1][0] == Catch::Approx(1.5f));

    // Prepared again, its callbacks are set again before it is initialized.
    REQUIRE(plugin->prepare({48000.0, 512, false}));
    time.position += 256;
    time.systemNanoseconds = nanosecondsNow();
    out = playAt(*plugin, 256, time);
    CHECK(out[0][0] == Catch::Approx(0.09f));
    REQUIRE(plugin->reset());
    plugin.reset();
    rig.main.settle();
    CHECK(misuses().empty());
}

TEST_CASE("A unit that won't take the program's rate is refused at it, saying so, and one it takes is set")
{
    Rig rig;
    auto plugin = rig.host.make(rig.format, rig.describe("Rate"));
    REQUIRE(rig.main.turnUntil([&] { return plugin->state() != Plugin::State::making; }));
    CHECK_FALSE(plugin->prepare({44100.0, 512, false}));
    CHECK(plugin->problem().message == "It doesn't take 44100 Hz.");

    auto taking = rig.made("Rate", {96000.0, 512, false});
    const auto out = play(*taking, 128, 1);
    CHECK(out[0][0] == Catch::Approx(0.96f));
    CHECK(misuses().empty());
}

TEST_CASE("A unit that needs every block the same size is fed full blocks through a buffer, the delay reported as "
          "latency")
{
    Rig rig;
    auto plugin = rig.made("Fixd");
    CHECK(plugin->latency() == 0);

    // A short block shows it needs full ones; it is prepared again with the buffer's delay as its latency.
    play(*plugin, 100, 1);
    REQUIRE(rig.main.turnUntil([&] { return plugin->latency() == 512; }, 5s));

    // From then on, what goes in comes out 512 frames later, whatever size the blocks are.
    const auto out = play(*plugin, 100, 20);
    for (std::size_t i = 512; i < out[0].size(); ++i)
        if (out[0][i] != static_cast<float>(i - 511))
        {
            FAIL_CHECK("frame " << i << " is " << out[0][i]);
            break;
        }
    CHECK(misuses().empty());
}

TEST_CASE("A unit's change of latency is followed when it says so, never asked for while it renders")
{
    Rig rig;
    auto plugin = rig.made("Late");
    CHECK(plugin->latency() == 480);
    play(*plugin, 128, 60);
    REQUIRE(rig.main.turnUntil([&] { return plugin->latency() == 960; }, 5s));
    CHECK(misuses().empty());
}
