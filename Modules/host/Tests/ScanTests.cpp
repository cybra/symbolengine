#include "Scratch.h"
#include "TestMainThread.h"

#include <symbolengine/core/Files.h>
#include <symbolengine/core/Paths.h>
#include <symbolengine/core/System.h>
#include <symbolengine/host/PluginScanner.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace symbolengine;
using namespace std::chrono_literals;

namespace
{

const std::filesystem::path bundles = SYMBOLENGINE_MISBEHAVING;

// A scan of the test bundles into a list of the test's own, its main thread the test's.
struct Rig
{
    ScratchFolder scratch;
    TestMainThread main;
    PluginList list{{scratch.path / "Plugins.json", "1.0", nullptr, nullptr, nullptr}};
    std::unique_ptr<PluginScanner> scanner;
    std::vector<std::pair<std::filesystem::path, std::string>> steps;
    std::vector<std::pair<std::filesystem::path, bool>> asking;
    std::optional<PluginScanEnd> end;

    explicit Rig(std::chrono::milliseconds limit = 10s, int filesPerWorker = 50,
                 std::int64_t workerMemory = std::int64_t(2) * 1024 * 1024 * 1024,
                 std::filesystem::path program = SYMBOLENGINE_SCAN_WORKER,
                 std::vector<std::pair<std::string, std::string>> environment = {})
    {
        // A worker built with the address sanitizer leaves crashes to the system, as a worker without it does.
        environment.emplace_back("ASAN_OPTIONS", "handle_segv=0:handle_sigbus=0:handle_abort=0");
        scanner = std::make_unique<PluginScanner>(
            list, PluginScanner::Options{
                      {std::move(program), std::move(environment), scratch.path / "workers.txt", limit, main.post()},
                      PluginSetup{48000.0, 256, false},
                      filesPerWorker,
                      workerMemory});
        scanner->onProgress = [this](const std::filesystem::path& file, const std::string& step) {
            steps.emplace_back(file, step);
        };
        scanner->onFinished = [this](const PluginScanEnd& how) { end = how; };
        scanner->onAsking = [this](const std::filesystem::path& file, bool now) { asking.emplace_back(file, now); };
    }

    static PluginScanFile file(const std::string& name)
    {
        return {"VST3", bundles / (name + ".vst3"), {1, 1, "1.0.0", 0}, false, {std::string(system::architecture())}};
    }

    // Scans the bundles of these names, in order, and waits for the scan to end.
    bool scan(const std::vector<std::string>& names, std::chrono::milliseconds limit = 120s)
    {
        std::vector<PluginScanFile> files;
        for (const auto& name : names)
            files.push_back(file(name));
        end.reset();
        REQUIRE(scanner->scan(files));
        return main.turnUntil([this] { return end.has_value(); }, limit);
    }

    std::optional<PluginFile> recorded(const std::string& name) const { return list.file(bundles / (name + ".vst3")); }

    std::string output() const
    {
        std::error_code ignored;
        return files::read(scratch.path / "workers.txt", ignored);
    }
};

// Whether the file was found whole: its one plugin listed and nothing set aside.
bool foundWhole(const std::optional<PluginFile>& file)
{
    return file && file->complete && file->outcome == PluginFile::Outcome::found && file->plugins.size() == 1 &&
           file->setAside.empty();
}

}

TEST_CASE("Bundles that crash or abort as their code loads are set aside with why, and a good one after each is found")
{
    Rig rig;
    REQUIRE(rig.scan({"Crash on load", "Good A", "Abort on load", "Good B"}));
    INFO(rig.output());
    CHECK_FALSE(rig.end->stopped);
    CHECK(rig.end->problem.empty());

    for (const auto* name : {"Crash on load", "Abort on load"})
    {
        INFO(name);
        const auto file = rig.recorded(name);
        REQUIRE(file);
        CHECK(file->outcome == PluginFile::Outcome::setAside);
        REQUIRE(file->failure);
        CHECK(file->failure->step == "loading the code");
        CHECK(file->failure->programVersion == "1.0");
        CHECK(file->plugins.empty());
        CHECK_FALSE(rig.list.needsScan(file->path, file->stamp));
    }
#if defined(_WIN32)
    // Windows' loader catches a fault in a library's start-up code and fails the load with a reason of its own.
    CHECK(rig.recorded("Crash on load")->failure->kind == PluginFailure::Kind::refused);
    CHECK_FALSE(rig.recorded("Crash on load")->failure->detail.empty());
#else
    CHECK(rig.recorded("Crash on load")->failure->kind == PluginFailure::Kind::crashed);
#endif
    CHECK(foundWhole(rig.recorded("Good A")));
    CHECK(foundWhole(rig.recorded("Good B")));
}

TEST_CASE("Plugins that crash, abort, throw, overflow their stack, fail fast or end the process as they are made are "
          "each set aside with why, and a good one after each is found")
{
    Rig rig;
    const std::vector<std::string> bad{"Crash when made",    "Abort when made",     "Throw when made",
                                       "Overflow when made", "Fail fast when made", "Exit when made"};
    std::vector<std::string> names;
    for (const auto& name : bad)
    {
        names.push_back(name);
        names.push_back("Good A");
    }
    names.push_back("Good B");
    REQUIRE(rig.scan(names));
    INFO(rig.output());

    for (const auto& name : bad)
    {
        INFO(name);
        const auto file = rig.recorded(name);
        REQUIRE(file);
        CHECK(file->complete);
        CHECK(file->outcome == PluginFile::Outcome::found);
        CHECK(file->plugins.empty());
        REQUIRE(file->setAside.size() == 1);
        CHECK(file->setAside[0].failure.step.starts_with("making plugin 1 of 1"));
        CHECK(file->setAside[0].plugin.identity.format == "VST3");
        CHECK(file->setAside[0].plugin.identity.id.size() == 32);
    }
    CHECK(rig.recorded("Throw when made")->setAside[0].failure.kind == PluginFailure::Kind::refused);
    CHECK(rig.recorded("Exit when made")->setAside[0].failure.kind == PluginFailure::Kind::noAnswer);
    CHECK(foundWhole(rig.recorded("Good A")));
    CHECK(foundWhole(rig.recorded("Good B")));
    CHECK(rig.list.find(rig.recorded("Crash when made")->setAside[0].plugin.identity) == std::nullopt);
}

TEST_CASE("Bundles that hang as their code loads, on the main thread or a thread it waits for, are timed out and set "
          "aside, their workers gone")
{
    Rig rig(1000ms);
    const auto asked = std::chrono::steady_clock::now();
    REQUIRE(rig.scan({"Hang on load", "Hang on a thread on load", "Good A"}));
    INFO(rig.output());

    // Each hangs twice at most, in a used worker and then a fresh one, and is cut off within the limit each time.
    CHECK(std::chrono::steady_clock::now() - asked < 12s);
    for (const auto* name : {"Hang on load", "Hang on a thread on load"})
    {
        INFO(name);
        const auto file = rig.recorded(name);
        REQUIRE(file);
        CHECK(file->outcome == PluginFile::Outcome::setAside);
        REQUIRE(file->failure);
        CHECK(file->failure->kind == PluginFailure::Kind::timedOut);
        CHECK(file->failure->step == "loading the code");
    }
    CHECK(foundWhole(rig.recorded("Good A")));
    CHECK(rig.scanner->workersKilled() == 2);
}

TEST_CASE("A module of many slow plugins one of which crashes gives every other one found and that one set aside")
{
    Rig rig(500ms);
    REQUIRE(rig.scan({"Good A", "Many 40 with 20 crashes 50 ms"}));
    INFO(rig.output());
    const auto file = rig.recorded("Many 40 with 20 crashes 50 ms");
    REQUIRE(file);
    CHECK(file->complete);
    CHECK(file->plugins.size() == 39);
    REQUIRE(file->setAside.size() == 1);
    CHECK(file->setAside[0].plugin.name == "Plugin 20");
    CHECK(file->setAside[0].failure.step == "making plugin 20 of 40, Plugin 20");
    CHECK(std::none_of(file->plugins.begin(), file->plugins.end(),
                       [](const PluginDescription& plugin) { return plugin.name == "Plugin 20"; }));

    // Tried again in a fresh worker and carried on past the crash, it was never made again from its first plugin.
    CHECK(std::count_if(rig.steps.begin(), rig.steps.end(), [&](const auto& step) {
              return step.first == file->path && step.second.starts_with("making plugin 1 of 40");
          }) == 1);
}

TEST_CASE("Damage a plugin leaves in its worker is never blamed on the next, which is tried again in a fresh worker")
{
    Rig rig(10s);
    REQUIRE(rig.scan({"Damage", "Slow A 1500 ms", "Slow B 1500 ms", "Slow C 1500 ms"}));
    INFO(rig.output());
    for (const auto* name : {"Damage", "Slow A 1500 ms", "Slow B 1500 ms", "Slow C 1500 ms"})
    {
        INFO(name);
        CHECK(foundWhole(rig.recorded(name)));
    }
    CHECK(rig.scanner->workersStarted() >= 2);
}

TEST_CASE("A worker that can't be started stops the scan after the third try, blaming no plugin")
{
    Rig rig(10s, 50, std::int64_t(2) * 1024 * 1024 * 1024, "/nowhere/no such worker");
    REQUIRE(rig.scan({"Good A", "Good B"}));
    CHECK_FALSE(rig.end->stopped);
    CHECK(rig.end->problem.find("no such worker") != std::string::npos);
    CHECK(rig.scanner->workersStarted() == 0);
    CHECK(rig.list.files().empty());
}

TEST_CASE("A worker that ends before saying anything of a request blames no plugin, and three in a row stop the scan")
{
    Rig rig(10s, 50, std::int64_t(2) * 1024 * 1024 * 1024, SYMBOLENGINE_SCAN_WORKER,
            {{"SYMBOLENGINE_TEST_WORKER_MODE", "silent"}});
    REQUIRE(rig.scan({"Good A", "Good B"}));
    CHECK_FALSE(rig.end->problem.empty());
    CHECK(rig.scanner->workersStarted() == 3);
    CHECK(rig.list.files().empty());
}

TEST_CASE("A worker is replaced after its set number of files, and once it uses more memory than set")
{
    SECTION("By its files")
    {
        Rig rig(10s, 2);
        REQUIRE(rig.scan({"Good A", "Good B", "Good C", "Slow A 1500 ms", "Detach deadlock"}));
        CHECK(rig.scanner->workersStarted() == 3);
        CHECK(rig.list.plugins().size() == 5);
    }

    SECTION("By its memory")
    {
        Rig rig(10s, 50, std::int64_t(200) * 1024 * 1024);
        REQUIRE(rig.scan({"Good A", "Hungry 300", "Good B", "Good C"}));
        INFO(rig.output());
        CHECK(rig.scanner->workersStarted() == 2);
        CHECK(rig.list.plugins().size() == 4);
    }
}

TEST_CASE("A worker whose plugin's code would deadlock as it is unloaded ends at once when its link is closed")
{
    Rig rig(10s, 1);
    const auto started = std::chrono::steady_clock::now();
    REQUIRE(rig.scan({"Detach deadlock", "Good A"}));
    CHECK(foundWhole(rig.recorded("Detach deadlock")));
    CHECK(foundWhole(rig.recorded("Good A")));

    // The first worker was let go after its one file and the second after the scan, each ending by itself.
    CHECK(rig.scanner->workersStarted() == 2);
    CHECK(rig.scanner->workersKilled() == 0);
    CHECK(std::chrono::steady_clock::now() - started < 4s);
}

TEST_CASE("Stopping a scan fails nothing: the file in hand is left to be scanned again")
{
    Rig rig(60s);
    REQUIRE(rig.scanner->scan({Rig::file("Good A"), Rig::file("Hang when made"), Rig::file("Good B")}));
    REQUIRE(rig.main.turnUntil(
        [&] {
            return rig.recorded("Good A").has_value() && !rig.steps.empty() &&
                   rig.steps.back().second.starts_with("making");
        },
        20s));
    rig.main.turnUntil([] { return false; }, 300ms);
    rig.scanner->stop();
    REQUIRE(rig.main.turnUntil([&] { return rig.end.has_value(); }, 10s));
    CHECK(rig.end->stopped);
    CHECK_FALSE(rig.scanner->isScanning());

    const auto hanging = rig.recorded("Hang when made");
    REQUIRE(hanging);
    CHECK_FALSE(hanging->complete);
    CHECK(hanging->setAside.empty());
    CHECK(rig.list.needsScan(hanging->path, hanging->stamp));
    CHECK_FALSE(rig.recorded("Good B"));
    CHECK(foundWhole(rig.recorded("Good A")));
}

TEST_CASE("Files the list says need no scan are passed over")
{
    Rig rig;
    REQUIRE(rig.scan({"Good A"}));
    REQUIRE(foundWhole(rig.recorded("Good A")));
    const auto started = rig.scanner->workersStarted();
    REQUIRE(rig.scan({"Good A"}));
    CHECK(rig.scanner->workersStarted() == started);
    CHECK(rig.end->problem.empty());
}

std::string frontmostExecutable();

// Whether a scan worker of the test program is the application in front, where the system says.
std::optional<bool> frontmostWorker()
{
#if defined(__APPLE__)
    std::error_code error;
    return std::filesystem::equivalent(frontmostExecutable(), SYMBOLENGINE_SCAN_WORKER, error);
#else
    return std::nullopt;
#endif
}

// The file name a frame inside the test bundle's code is shown with in a backtrace.
std::string bundleBinary([[maybe_unused]] const std::string& name)
{
#if defined(__APPLE__)
    return "SymbolEngine Misbehaving";
#else
    return name;
#endif
}

TEST_CASE("A worker's crash leaves a note of the file and step it was on, the signal and where in the code it came")
{
    Rig rig;
    REQUIRE(rig.scan({"Crash on a thread when made"}));
    INFO(rig.output());
    const auto file = rig.recorded("Crash on a thread when made");
    REQUIRE(file);
    REQUIRE(file->setAside.size() == 1);
    const auto& failure = file->setAside[0].failure;
    INFO(failure.detail);
    CHECK(failure.kind == PluginFailure::Kind::crashed);
    CHECK(failure.detail.find("making plugin 1 of 1") != std::string::npos);
    CHECK(failure.detail.find("Crash on a thread when made.vst3") != std::string::npos);
#if defined(_WIN32)
    CHECK(failure.detail.find("0xC0000005") != std::string::npos);
#else
    CHECK((failure.detail.find("SIGSEGV") != std::string::npos || failure.detail.find("SIGBUS") != std::string::npos));
#endif
    // A frame of the backtrace inside the plugin's own code.
    const auto backtrace = failure.detail.substr(failure.detail.find('\n') + 1);
    CHECK(backtrace.find(bundleBinary("Crash on a thread when made")) != std::string::npos);
}

#if defined(__APPLE__) || defined(_WIN32)
TEST_CASE("A plugin that asks something as it is made is waited for past the limit, and then found")
{
    Rig rig(1000ms);
    // Whether the worker's window was in front at any moment while it asked, as someone using the computer meanwhile
    // can bring another window in front of it again: on a computer in use, a click at the wrong moment can still fail
    // this, which continuous integration's computers, used by no one, never see.
    std::optional<bool> front;
    rig.scanner->onAsking = [&](const std::filesystem::path& file, bool now) {
        rig.asking.emplace_back(file, now);
        if (now && rig.asking.size() == 1)
            rig.main.post()([&] {
                rig.main.turnUntil(
                    [&] {
                        front = frontmostWorker();
                        return front.value_or(true);
                    },
                    2800ms);
            });
    };
    REQUIRE(rig.scan({"Asks when made 3000 ms", "Asks modally when made 3000 ms", "Good A"}));
    #if defined(__APPLE__)
    // The first question's window above every other application's, whichever is the application in front.
    REQUIRE(front);
    CHECK(*front);
    #endif
    INFO(rig.output());
    std::string said;
    for (const auto& [file, now] : rig.asking)
        said += paths::toUtf8(file.filename()) + (now ? " asking; " : " done asking; ");
    INFO(said);
    CHECK(foundWhole(rig.recorded("Asks when made 3000 ms")));
    CHECK(foundWhole(rig.recorded("Asks modally when made 3000 ms")));
    CHECK(foundWhole(rig.recorded("Good A")));
    REQUIRE(rig.asking.size() == 4);
    CHECK(rig.asking[0].first == Rig::file("Asks when made 3000 ms").path);
    CHECK(rig.asking[0].second);
    CHECK_FALSE(rig.asking[1].second);
    CHECK(rig.asking[2].first == Rig::file("Asks modally when made 3000 ms").path);
    CHECK(rig.asking[2].second);
    CHECK_FALSE(rig.asking[3].second);
    CHECK(rig.scanner->workersKilled() == 0);
}

TEST_CASE("Skip ends the worker of a plugin that is asking something, leaving it to be scanned again, never set aside")
{
    Rig rig(1000ms);
    REQUIRE(rig.scanner->scan({Rig::file("Asks when made 60000 ms"), Rig::file("Good A")}));
    REQUIRE(rig.main.turnUntil([&] { return !rig.asking.empty(); }, 20s));
    const auto skipped = std::chrono::steady_clock::now();
    rig.scanner->skip();
    REQUIRE(rig.main.turnUntil([&] { return rig.end.has_value(); }, 20s));
    CHECK(std::chrono::steady_clock::now() - skipped < 5s);
    CHECK_FALSE(rig.end->stopped);

    const auto asked = rig.recorded("Asks when made 60000 ms");
    REQUIRE(asked);
    CHECK_FALSE(asked->complete);
    CHECK(asked->setAside.empty());
    CHECK(asked->plugins.empty());
    CHECK(rig.list.needsScan(asked->path, asked->stamp));
    CHECK(foundWhole(rig.recorded("Good A")));
}
#endif
