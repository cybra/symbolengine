#include <symbolengine/core/ChildProcess.h>
#include <symbolengine/core/Files.h>
#include <symbolengine/host/FakePluginFormat.h>
#include <symbolengine/host/Plugin.h>
#include <symbolengine/host/PluginCrashNote.h>

#include "CrashNoteRecords.h"
#include "Scratch.h"
#include "TestMainThread.h"
#include "Vst3/TestIds.h"

#include <catch2/catch_test_macros.hpp>

#include "CrashNoteRecords.h"
#include <algorithm>
#include <atomic>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace symbolengine;
using namespace std::chrono_literals;

namespace
{

const std::filesystem::path module = SYMBOLENGINE_TEST_VST3;

// The crash note program run with its note in the folder, ending as asked: one that hangs is killed once the call is
// under way. Gives how it ended, and the note as the program's next run reads it.
struct Run
{
    std::optional<ProcessEnding> ending;
    PluginRunEnding last;
};

Run run(const std::filesystem::path& folder, const std::string& how)
{
    const auto note = folder / "Plugin crash note";
    auto marker = note;
    marker += ".ready";
    ChildProcess::Options options;
    options.arguments = {note.string(), module.string(), how};
    options.outputFile = folder / "output.txt";
    // A sanitizer would catch the crash itself, before the program's handler.
    options.environment = {{"ASAN_OPTIONS", "handle_segv=0:handle_sigbus=0:handle_sigill=0:handle_sigfpe=0"}};
    std::error_code started;
    auto program = ChildProcess::start(SYMBOLENGINE_CRASH_NOTE, options, started);
    REQUIRE(program != nullptr);

    Run made;

    if (how.ends_with("-hang"))
    {
        const auto until = std::chrono::steady_clock::now() + 30s;
        std::error_code missing;
        while (!std::filesystem::exists(marker, missing) && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(10ms);
        REQUIRE(std::filesystem::exists(marker, missing));

        // Long enough for the call to have begun after the marker was written.
        std::this_thread::sleep_for(300ms);
        REQUIRE(program->kill());
    }

    made.ending = program->wait(30s);
    REQUIRE(made.ending.has_value());
    PluginError error;
    auto next = PluginCrashNote::open(note, false, error);
    REQUIRE(next != nullptr);
    made.last = next->lastRun();
    return made;
}

}

TEST_CASE(
    "A run killed in the middle of a call into a plugin leaves a note naming the plugin and the call, on the main "
    "thread or the audio thread, read at the next start")
{
    ScratchFolder folder;

    SECTION("on the main thread")
    {
        const auto ended = run(folder.path, "main-hang");
        CHECK(ended.ending->killed);
        CHECK_FALSE(ended.last.clean);
        CHECK(ended.last.context == "Tuesday's interview.symbol");
        REQUIRE(ended.last.culprit() != nullptr);
        CHECK(ended.last.culprit()->plugin.identity == PluginIdentity{"VST3", testIds::faulting});
        CHECK(ended.last.culprit()->plugin.location == module);
        CHECK(ended.last.culprit()->call == "setParameter");
        CHECK_FALSE(ended.last.culprit()->audioThread);
    }

    SECTION("on the audio thread")
    {
        const auto ended = run(folder.path, "audio-hang");
        CHECK(ended.ending->killed);
        CHECK_FALSE(ended.last.clean);
        REQUIRE(ended.last.culprit() != nullptr);
        CHECK(ended.last.culprit()->plugin.identity == PluginIdentity{"VST3", testIds::faulting});
        CHECK(ended.last.culprit()->call == "process");
        CHECK(ended.last.culprit()->audioThread);
    }
}

#if !defined(_WIN32)
TEST_CASE(
    "A run that crashes inside a plugin has the crash, a backtrace in the plugin's code and the thread it came on "
    "added to its note, and the crash goes on to end it as it would have")
{
    ScratchFolder folder;

    for (const auto& how : {std::string("main-crash"), std::string("audio-crash")})
    {
        INFO(how);
        const auto ended = run(folder.path, how);
        CHECK(ended.ending->kind == ProcessEnding::Kind::signal);
        CHECK_FALSE(ended.last.clean);
        REQUIRE(ended.last.culprit() != nullptr);
        CHECK(ended.last.culprit()->crashed);
        CHECK(ended.last.culprit()->plugin.identity == PluginIdentity{"VST3", testIds::faulting});
        CHECK(ended.last.culprit()->audioThread == (how == "audio-crash"));
        CHECK(ended.last.crash.find("SIGSEGV") != std::string::npos);
        CHECK(ended.last.crash.find("SymbolEngine Test") != std::string::npos);
    }
}
#endif

TEST_CASE("A clean end, or one outside every call into a plugin, blames no plugin, and a damaged note reads as nothing")
{
    ScratchFolder folder;

    const auto clean = run(folder.path, "clean");
    CHECK(clean.ending->kind == ProcessEnding::Kind::exited);
    CHECK(clean.ending->code == 0);
    CHECK(clean.last.clean);
    CHECK(clean.last.culprit() == nullptr);

    const auto outside = run(folder.path, "outside");
    CHECK(outside.ending->kind != ProcessEnding::Kind::exited);
    CHECK_FALSE(outside.last.clean);
    CHECK(outside.last.inside.empty());
    CHECK(outside.last.culprit() == nullptr);

    // A real note of a run still going, cut short; a few bytes; and bytes of nothing like a note: nothing read, and the
    // note begun again.
    std::string half;
    {
        PluginError error;
        auto going = PluginCrashNote::open(folder.path / "Going", false, error);
        REQUIRE(going != nullptr);
        going->setContext("A project");
        std::error_code missing;
        const auto whole = files::read(folder.path / "Going", missing);
        REQUIRE(whole.size() > 1000);
        half = whole.substr(0, whole.size() / 2);
    }
    const auto note = folder.path / "Damaged note";
    for (const auto& damage : {half, std::string("SECN"), std::string(70000, '\x7f')})
    {
        files::writeSafely(note, damage);
        PluginError error;
        auto opened = PluginCrashNote::open(note, false, error);
        REQUIRE(opened != nullptr);
        CHECK(opened->lastRun().clean);
        CHECK(opened->lastRun().inside.empty());
    }
}

TEST_CASE(
    "The note's record of each call costs under a fifth of a call into the quickest plugin, so it can stand around "
    "every block of audio",
    "[timing]")
{
    ScratchFolder folder;
    PluginError error;
    auto note = PluginCrashNote::open(folder.path / "Note", false, error);
    REQUIRE(note != nullptr);

    // The same plugin played in a host that records each call in the note and in one that doesn't, short runs of
    // blocks taken from each in turn, so a machine that slows down or speeds up as it runs slows both alike, and the
    // fastest run of each kept.
    TestMainThread main;
    FakePluginFormat fake{main.isCurrent(), main.post()};
    PluginHost plainHost{{main.post(), main.isCurrent(), 2000ms, nullptr}};
    PluginHost notedHost{{main.post(), main.isCurrent(), 2000ms, nullptr}};
    notedHost.setCrashNote(note.get());
    const auto make = [&](PluginHost& host) {
        auto plugin = host.make(fake, FakePluginFormat::describe("quick"));
        REQUIRE(main.turnUntil([&] { return plugin->state() != Plugin::State::making; }));
        REQUIRE(plugin->prepare({48000.0, 16, false}));
        return plugin;
    };
    const auto plainPlugin = make(plainHost);
    const auto notedPlugin = make(notedHost);

    float left[16] = {}, right[16] = {};
    float* channels[] = {left, right};
    constexpr int blocks = 5000;
    const auto timeRun = [&](Plugin& plugin) {
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < blocks; ++i)
            plugin.process(channels, 2, 16);
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start) / blocks;
    };
    auto plain = std::chrono::nanoseconds::max(), noted = std::chrono::nanoseconds::max();
    for (int attempt = 0; attempt < 100; ++attempt)
    {
        plain = std::min(plain, timeRun(*plainPlugin));
        noted = std::min(noted, timeRun(*notedPlugin));
    }
    plainPlugin->release();
    notedPlugin->release();

    // Measured against the call itself, a few tens of billionths of a second to a call's thousand on a Mac and about a
    // hundred on Linux's runner, as a slower machine or the sanitizers slow both alike.
    INFO("a block takes " << plain.count() << " ns, " << noted.count() << " ns with the note");
    CHECK(noted - plain < plain / 5);
}

TEST_CASE("A crash outside every call into a plugin blames none, whatever the audio thread was playing")
{
    ScratchFolder folder;
    const auto ended = run(folder.path, "outside-while-playing");
    CHECK_FALSE(ended.last.clean);
    CHECK(ended.last.crash.find("SIGSEGV") != std::string::npos);
    REQUIRE(ended.last.inside.size() == 1);
    CHECK(ended.last.inside[0].audioThread);
    CHECK(ended.last.culprit() == nullptr);
}

TEST_CASE("A note has a place for a new thread however many have come and gone")
{
    ScratchFolder folder;
    PluginError error;
    const auto file = folder.path / "Note";
    auto note = PluginCrashNote::open(file, false, error);
    REQUIRE(note != nullptr);
    PluginDescription plugin;
    plugin.identity = {"VST3", "A"};
    plugin.name = "A plugin";
    const auto slot = crashNote::claim(note.get(), plugin);
    REQUIRE(slot >= 0);

    // More than the note has room for, all still there so each is a thread of its own, and none of them in a call now.
    std::vector<std::thread> gone;
    std::atomic<int> called{0};
    std::atomic<bool> go{false};
    for (int thread = 0; thread < 3 * crashNote::threadCount; ++thread)
        gone.emplace_back([&] {
            {
                crashNote::InCall call(note.get(), slot, "process", true);
            }
            ++called;
            while (!go)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        });
    while (called < 3 * crashNote::threadCount)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    // The latest is inside its call as the file is read, as it would be after a crash.
    std::atomic<bool> inside{false}, letGo{false};
    std::thread latest([&] {
        crashNote::InCall call(note.get(), slot, "setState", false);
        inside = true;
        while (!letGo)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    });
    while (!inside)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const auto copy = folder.path / "Copy";
    std::filesystem::copy_file(file, copy);
    letGo = true;
    latest.join();
    go = true;
    for (auto& thread : gone)
        thread.join();

    auto reading = PluginCrashNote::open(copy, false, error);
    REQUIRE(reading != nullptr);
    REQUIRE(reading->lastRun().inside.size() == 1);
    CHECK(reading->lastRun().inside[0].call == "setState");
}

TEST_CASE("A second copy of the program neither reads the first's note as its crash nor harms it")
{
    ScratchFolder folder;
    PluginError error;
    const auto file = folder.path / "Note";

    auto first = PluginCrashNote::open(file, false, error);
    REQUIRE(first != nullptr);
    first->setContext("The first copy");
    PluginDescription plugin;
    plugin.identity = {"VST3", "A"};
    const auto slot = crashNote::claim(first.get(), plugin);
    REQUIRE(slot >= 0);

    // Running, the first's note looks like the note of a crash to anything that reads it.
    {
        auto second = PluginCrashNote::open(file, false, error);
        REQUIRE(second != nullptr);
        CHECK(second->lastRun().clean);
        second->setContext("The second copy");
        const auto theirs = crashNote::claim(second.get(), plugin);
        CHECK(theirs >= 0);
        crashNote::InCall call(second.get(), theirs, "process", true);
    }

    // The first's still holds, and it ended well.
    {
        crashNote::InCall call(first.get(), slot, "setState", false);
        const auto copy = folder.path / "Copy";
        std::filesystem::copy_file(file, copy);
        auto reading = PluginCrashNote::open(copy, false, error);
        REQUIRE(reading != nullptr);
        CHECK(reading->lastRun().context == "The first copy");
        REQUIRE(reading->lastRun().inside.size() == 1);
    }
    first.reset();
    auto after = PluginCrashNote::open(file, false, error);
    REQUIRE(after != nullptr);
    CHECK(after->lastRun().clean);
}
