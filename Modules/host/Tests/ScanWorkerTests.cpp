#include "Scratch.h"
#include "TestMainThread.h"
#include "Vst3/TestIds.h"

#include <symbolengine/core/ChildProcess.h>
#include <symbolengine/core/Files.h>
#include <symbolengine/core/Paths.h>
#include <symbolengine/host/PluginScanWorker.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
    #include <windows.h>
#else
    #include <cerrno>
    #include <signal.h>
#endif

using namespace symbolengine;
using namespace std::chrono_literals;

bool showsInDock(std::int64_t processId);

namespace
{

using Clock = std::chrono::steady_clock;

bool processExists(std::int64_t id)
{
#if defined(_WIN32)
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(id));
    if (process == nullptr)
        return false;
    DWORD code = 0;
    const bool running = GetExitCodeProcess(process, &code) && code == STILL_ACTIVE;
    CloseHandle(process);
    return running;
#else
    return ::kill(static_cast<pid_t>(id), 0) == 0 || errno != ESRCH;
#endif
}

// Waits for a process to be gone, giving whether it went within the limit.
bool goneWithin(std::int64_t id, std::chrono::milliseconds limit)
{
    const auto until = Clock::now() + limit;
    while (processExists(id))
    {
        if (Clock::now() > until)
            return false;
        std::this_thread::sleep_for(10ms);
    }
    return true;
}

// A scan worker of the test program, its main thread the test's, with all it says kept.
struct Scan
{
    ScratchFolder scratch;
    TestMainThread main;
    std::unique_ptr<PluginScanWorker> worker;
    std::vector<std::string> steps;
    std::vector<PluginDescription> plugins;
    std::vector<PluginSetAside> setAside;
    std::vector<std::pair<PluginFile::Outcome, std::optional<PluginFailure>>> done;
    std::vector<PluginFailure> failed;
    std::vector<std::optional<PluginDescription>> inHand;
    int files = 0;

    explicit Scan(std::chrono::milliseconds limit = 20s,
                  std::vector<std::pair<std::string, std::string>> environment = {})
    {
        std::error_code error;
        worker = PluginScanWorker::start(
            {SYMBOLENGINE_SCAN_WORKER, std::move(environment), scratch.path / "worker.txt", limit, main.post()}, error);
        REQUIRE(worker);
        worker->onProgress = [this](const std::string& step) { steps.push_back(step); };
        worker->onPlugin = [this](const PluginDescription& plugin) { plugins.push_back(plugin); };
        worker->onSetAside = [this](const PluginSetAside& plugin) { setAside.push_back(plugin); };
        worker->onDone = [this](PluginFile::Outcome outcome, const std::optional<PluginFailure>& why) {
            done.emplace_back(outcome, why);
        };
        worker->onFailed = [this](const PluginFailure& failure, const std::optional<PluginDescription>& plugin) {
            failed.push_back(failure);
            inHand.push_back(plugin);
        };
    }

    // A fake plugin file holding what the JSON says.
    std::filesystem::path fake(const std::string& contents)
    {
        const auto file = scratch.path / ("Fake " + std::to_string(++files) + ".json");
        files::writeSafely(file, contents);
        return file;
    }

    bool scan(const std::filesystem::path& file, std::vector<PluginIdentity> skip = {}, std::string format = "Fake")
    {
        return worker->scan({std::move(format), file, PluginSetup{48000.0, 512, false}, std::move(skip)});
    }

    // Turns the main thread until the request under way is answered or the worker has failed.
    bool finished(std::chrono::milliseconds limit = 30s)
    {
        const auto answers = done.size() + failed.size();
        return main.turnUntil([&] { return done.size() + failed.size() > answers; }, limit);
    }

    std::vector<std::string> names() const
    {
        std::vector<std::string> all;
        for (const auto& plugin : plugins)
            all.push_back(plugin.name);
        return all;
    }

    std::string output() const
    {
        std::error_code ignored;
        return files::read(scratch.path / "worker.txt", ignored);
    }
};

}

TEST_CASE("A worker scans a file plugin by plugin, answering each as it finishes and the file once it is done")
{
    Scan scan;
    const auto file = scan.fake(R"({"plugins": [
        {"id": "alpha", "name": "Alpha"},
        {"id": "beta", "name": "Beta", "kind": "instrument", "does": "crash", "at": "arrange"},
        {"id": "gamma", "name": "Gamma", "takes": [[2, 0]]},
        {"id": "delta", "name": "Delta", "takes": [[1, 1]]}]})");
    REQUIRE(scan.scan(file));
    CHECK(scan.worker->isBusy());
    REQUIRE(scan.finished());
    INFO(scan.output());

    CHECK(scan.failed.empty());
    REQUIRE(scan.done.size() == 1);
    CHECK(scan.done[0].first == PluginFile::Outcome::found);
    CHECK_FALSE(scan.done[0].second);
    CHECK_FALSE(scan.worker->isBusy());

    // The instrument is answered as it declares itself, never made, so its crash never comes; the plugin with no audio
    // out is found as that, never set aside.
    REQUIRE(scan.names() == std::vector<std::string>{"Alpha", "Beta", "Gamma", "Delta"});
    CHECK(scan.plugins[0].identity == PluginIdentity{"Fake", "alpha"});
    CHECK(scan.plugins[0].location == file);
    CHECK(scan.plugins[0].maker == "Fake Maker");
    CHECK(scan.plugins[0].sdk == "Fake 1");
    CHECK(scan.plugins[0].kind == PluginKind::effect);
    CHECK(scan.plugins[0].layouts == std::vector<PluginLayout>{{2, 2}});
    CHECK(scan.plugins[1].kind == PluginKind::instrument);
    CHECK(scan.plugins[1].layouts.empty());
    CHECK(scan.plugins[2].layouts == std::vector<PluginLayout>{{2, 0}});
    CHECK(scan.plugins[3].layouts == std::vector<PluginLayout>{{1, 1}});
    CHECK(scan.setAside.empty());

    CHECK(std::find(scan.steps.begin(), scan.steps.end(), "loading the code") != scan.steps.end());
    CHECK(std::find(scan.steps.begin(), scan.steps.end(), "making plugin 3 of 4, Gamma") != scan.steps.end());
}

TEST_CASE("A file holding no plugins, and one whose code won't load, are each said as such by one worker in turn")
{
    Scan scan;
    REQUIRE(scan.scan(scan.fake(R"({"plugins": []})")));
    REQUIRE(scan.finished());
    REQUIRE(scan.done.size() == 1);
    CHECK(scan.done[0].first == PluginFile::Outcome::nothing);

    REQUIRE(scan.scan(scan.fake(R"({"error": "Its code needs a library that isn't there."})")));
    REQUIRE(scan.finished());
    REQUIRE(scan.done.size() == 2);
    CHECK(scan.done[1].first == PluginFile::Outcome::setAside);
    REQUIRE(scan.done[1].second);
    CHECK(scan.done[1].second->kind == PluginFailure::Kind::refused);
    CHECK(scan.done[1].second->step == "loading the code");
    CHECK(scan.done[1].second->detail == "Its code needs a library that isn't there.");

    // A format the worker doesn't have is refused the same way.
    REQUIRE(scan.scan(scan.fake(R"({"plugins": []})"), {}, "Elsewhere"));
    REQUIRE(scan.finished());
    REQUIRE(scan.done.size() == 3);
    CHECK(scan.done[2].first == PluginFile::Outcome::setAside);
    CHECK(scan.failed.empty());
    CHECK(scan.plugins.empty());
}

TEST_CASE("A plugin that can't be made or set up is set aside with the step it came at, and the others are found")
{
    Scan scan;
    REQUIRE(scan.scan(scan.fake(R"({"plugins": [
        {"id": "alpha", "name": "Alpha", "failToMake": true},
        {"id": "beta", "name": "Beta", "failToActivate": true},
        {"id": "gamma", "name": "Gamma"}]})")));
    REQUIRE(scan.finished());
    INFO(scan.output());

    CHECK(scan.names() == std::vector<std::string>{"Gamma"});
    REQUIRE(scan.setAside.size() == 2);
    CHECK(scan.setAside[0].plugin.name == "Alpha");
    CHECK(scan.setAside[0].failure.kind == PluginFailure::Kind::refused);
    CHECK(scan.setAside[0].failure.step == "making plugin 1 of 3, Alpha");
    CHECK_FALSE(scan.setAside[0].failure.detail.empty());
    CHECK(scan.setAside[1].plugin.name == "Beta");
    CHECK(scan.setAside[1].failure.step == "setting up plugin 2 of 3, Beta");
    REQUIRE(scan.done.size() == 1);
    CHECK(scan.done[0].first == PluginFile::Outcome::found);
}

TEST_CASE("Plugins already answered are skipped, as when a scan carries on past one that crashed")
{
    Scan scan;
    REQUIRE(scan.scan(scan.fake(R"({"plugins": [
        {"id": "alpha", "name": "Alpha", "does": "crash"},
        {"id": "beta", "name": "Beta"}]})"),
                      {{"Fake", "alpha"}}));
    REQUIRE(scan.finished());
    CHECK(scan.failed.empty());
    CHECK(scan.names() == std::vector<std::string>{"Beta"});
}

TEST_CASE("A worker that makes no progress is killed within its limit, and is gone")
{
    const auto how = GENERATE(std::string("hang"), std::string("hangOnThread"));
    Scan scan(1000ms);
    const auto id = scan.worker->processId();
    REQUIRE(scan.scan(scan.fake(R"({"plugins": [{"id": "alpha", "name": "Alpha", "does": ")" + how + R"("}]})")));
    const auto asked = Clock::now();
    REQUIRE(scan.finished());

    CHECK(Clock::now() - asked < 3s);
    REQUIRE(scan.failed.size() == 1);
    CHECK(scan.failed[0].kind == PluginFailure::Kind::timedOut);
    CHECK(scan.failed[0].step == "making plugin 1 of 1, Alpha");
    CHECK(scan.failed[0].code == 1000);
    REQUIRE(scan.inHand[0]);
    CHECK(scan.inHand[0]->name == "Alpha");
    CHECK(scan.worker->hasEnded());
    CHECK(goneWithin(id, 2s));
    CHECK_FALSE(scan.scan(scan.fake(R"({"plugins": []})")));
}

TEST_CASE("A file of many slow plugins is never timed out while each makes progress")
{
    std::string plugins;
    for (int i = 0; i < 30; ++i)
        plugins += std::string(i == 0 ? "" : ",") + R"({"id": "slow)" + std::to_string(i) +
                   R"(", "name": "Slow", "does": "sleep", "ms": 100})";
    Scan scan(500ms);
    REQUIRE(scan.scan(scan.fake(R"({"plugins": [)" + plugins + "]}")));
    REQUIRE(scan.finished());
    INFO(scan.output());
    CHECK(scan.failed.empty());
    CHECK(scan.plugins.size() == 30);
    REQUIRE(scan.done.size() == 1);
}

TEST_CASE("A plugin slow at every step is never timed out while each step begins within the limit")
{
    Scan scan(500ms);
    REQUIRE(scan.scan(scan.fake(R"({"plugins": [{"id": "alpha", "name": "Alpha",
        "sleeps": {"arrange": 300, "activate": 300, "startProcessing": 300}}]})")));
    REQUIRE(scan.finished());
    INFO(scan.output());
    CHECK(scan.failed.empty());
    CHECK(scan.names() == std::vector<std::string>{"Alpha"});
}

TEST_CASE("A worker that answers and ends at once has answered: all it sent is read before its ending is judged")
{
    Scan scan(20s, {{"SYMBOLENGINE_TEST_WORKER_MODE", "answersThenEnds"}});
    REQUIRE(scan.scan(scan.fake(R"({"plugins": []})")));
    REQUIRE(scan.finished());
    CHECK(scan.failed.empty());
    CHECK(scan.plugins.size() == 5000);
    REQUIRE(scan.done.size() == 1);
    CHECK(scan.done[0].first == PluginFile::Outcome::found);
}

TEST_CASE("A plugin that crashes takes down only its worker, which says how it ended and at which step")
{
    // A worker built with the address sanitizer leaves the crash to the system, as a worker without it does.
    Scan scan(20s, {{"ASAN_OPTIONS", "handle_segv=0:handle_sigbus=0"}});
    const auto id = scan.worker->processId();
    REQUIRE(scan.scan(scan.fake(R"({"plugins": [
        {"id": "alpha", "name": "Alpha"},
        {"id": "beta", "name": "Beta", "does": "crash", "at": "activate"}]})")));
    REQUIRE(scan.finished());

    CHECK(scan.names() == std::vector<std::string>{"Alpha"});
#if defined(_WIN32)
    // On Windows the host catches the fault inside the plugin's call, so the plugin is set aside as crashed and the
    // worker answers the file, to be let go by the scan after it, as a process that faulted can't be trusted.
    static_cast<void>(id);
    CHECK(scan.failed.empty());
    REQUIRE(scan.setAside.size() == 1);
    CHECK(scan.setAside[0].plugin.identity == PluginIdentity{"Fake", "beta"});
    CHECK(scan.setAside[0].failure.kind == PluginFailure::Kind::crashed);
    CHECK(scan.setAside[0].failure.code == 0xC0000005);
    CHECK(scan.setAside[0].failure.step == "setting up plugin 2 of 2, Beta");
    REQUIRE(scan.done.size() == 1);
#else
    CHECK(scan.done.empty());
    REQUIRE(scan.failed.size() == 1);
    CHECK(scan.failed[0].kind == PluginFailure::Kind::crashed);
    CHECK(scan.failed[0].step == "setting up plugin 2 of 2, Beta");
    REQUIRE(scan.inHand[0]);
    CHECK(scan.inHand[0]->identity == PluginIdentity{"Fake", "beta"});
    CHECK(scan.inHand[0]->location == scan.plugins[0].location);
    CHECK((scan.failed[0].code == SIGSEGV || scan.failed[0].code == SIGBUS));
    CHECK(scan.worker->hasEnded());
    CHECK(goneWithin(id, 2s));
#endif
}

TEST_CASE("A worker that ends without answering has failed, whatever its exit code")
{
    Scan scan;
    REQUIRE(scan.scan(scan.fake(R"({"plugins": [
        {"id": "alpha", "name": "Alpha"},
        {"id": "beta", "name": "Beta", "does": "exit", "at": "process"}]})")));
    REQUIRE(scan.finished());
    CHECK(scan.names() == std::vector<std::string>{"Alpha"});
    REQUIRE(scan.failed.size() == 1);
    CHECK(scan.failed[0].kind == PluginFailure::Kind::noAnswer);
    CHECK(scan.failed[0].step == "playing plugin 2 of 2, Beta");
    CHECK(scan.done.empty());
}

TEST_CASE("Answers that aren't right are never taken: another request's, cut short, empty, malformed, twice or none")
{
    const auto mode =
        GENERATE(std::string("wrongNumber"), std::string("cutShort"), std::string("empty"), std::string("notAPlugin"),
                 std::string("twice"), std::string("progressOnly"), std::string("silent"));
    INFO(mode);
    Scan scan(1500ms, {{"SYMBOLENGINE_TEST_WORKER_MODE", mode}});
    REQUIRE(scan.scan(scan.fake(R"({"plugins": []})")));
    REQUIRE(scan.finished());
    scan.main.turnUntil([] { return false; }, 300ms);

    if (mode == "twice")
    {
        // The first answers are taken; the second done and anything after it aren't.
        CHECK(scan.plugins.size() == 1);
        REQUIRE(scan.done.size() == 1);
        CHECK(scan.done[0].first == PluginFile::Outcome::found);
        CHECK(scan.failed.empty());
        return;
    }

    if (mode == "notAPlugin")
    {
        // Only the one whole plugin, and the one outcome it knows.
        CHECK(scan.names() == std::vector<std::string>{"Wrong"});
        REQUIRE(scan.done.size() == 1);
        CHECK(scan.done[0].first == PluginFile::Outcome::found);
        return;
    }
    CHECK(scan.plugins.empty());

    CHECK(scan.done.empty());
    REQUIRE(scan.failed.size() == 1);
    CHECK(scan.failed[0].kind ==
          (mode == "wrongNumber" ? PluginFailure::Kind::timedOut : PluginFailure::Kind::noAnswer));
    CHECK_FALSE(scan.inHand[0]);
    if (mode == "progressOnly")
        CHECK(scan.failed[0].step == "loading the code");
    if (mode == "silent")
        CHECK(scan.failed[0].step == "starting the scan");
}

TEST_CASE("A worker ends by itself once its link is closed, and once its program is killed or quits")
{
    SECTION("Its link closed")
    {
        Scan scan;
        REQUIRE(scan.scan(scan.fake(R"({"plugins": [{"id": "alpha", "name": "Alpha"}]})")));
        REQUIRE(scan.finished());
        const auto id = scan.worker->processId();
        const auto ended = scan.worker->close();
        CHECK_FALSE(ended.killed);
        CHECK(ended.kind == ProcessEnding::Kind::exited);
        CHECK(goneWithin(id, 2s));
    }

    for (const std::string how : {"killed", "quits"})
    {
        DYNAMIC_SECTION("Its program " << how)
        {
            ScratchFolder scratch;
            const auto idFile = scratch.path / "worker id.txt";
            const auto quitFile = scratch.path / "quit now";
            std::error_code error;
            ChildProcess::Options options;
            options.arguments = {how, paths::toUtf8(idFile), paths::toUtf8(quitFile)};
            auto program = ChildProcess::start(SYMBOLENGINE_SCAN_WORKER, options, error);
            REQUIRE(program);
            const auto until = Clock::now() + 10s;
            while (files::read(idFile, error).empty() && Clock::now() < until)
                std::this_thread::sleep_for(20ms);
            const auto id = std::stoll(files::read(idFile));
            REQUIRE(processExists(id));
            if (how == "killed")
                REQUIRE(program->kill());
            else
                files::writeSafely(quitFile, "");
            const auto stopped = Clock::now();
            CHECK(goneWithin(id, 2500ms));
            CHECK(Clock::now() - stopped < 2500ms);
        }
    }
}

TEST_CASE("A worker that can't be started says why, blaming no plugin")
{
    TestMainThread main;
    std::error_code error;
    auto worker = PluginScanWorker::start({"/nowhere/no such program", {}, {}, 1s, main.post()}, error);
    CHECK(worker == nullptr);
    CHECK(error);
}

TEST_CASE("A worker scans a VST3 module of many plugins, every effect made, played and taken down")
{
    Scan scan;
    REQUIRE(scan.scan(paths::fromUtf8(SYMBOLENGINE_TEST_VST3), {}, "VST3"));
    REQUIRE(scan.finished(60s));
    INFO(scan.output());
    CHECK(scan.failed.empty());
    REQUIRE(scan.done.size() == 1);
    CHECK(scan.done[0].first == PluginFile::Outcome::found);
    CHECK(scan.setAside.empty());

    const auto named = [&](const std::string& id) {
        return std::find_if(scan.plugins.begin(), scan.plugins.end(),
                            [&](const PluginDescription& plugin) { return plugin.identity.id == id; });
    };
    REQUIRE(named(testIds::gain) != scan.plugins.end());
    CHECK(named(testIds::gain)->layouts == std::vector<PluginLayout>{{2, 2}});
    CHECK(named(testIds::gain)->maker == "SymbolEngine");
    REQUIRE(named(testIds::mono) != scan.plugins.end());
    CHECK(named(testIds::mono)->layouts == std::vector<PluginLayout>{{1, 1}});
    REQUIRE(named(testIds::noOutput) != scan.plugins.end());
    CHECK(named(testIds::noOutput)->layouts.at(0).outputs == 0);
    REQUIRE(named(testIds::instrument) != scan.plugins.end());
    CHECK(named(testIds::instrument)->kind == PluginKind::instrument);
    CHECK(named(testIds::instrument)->layouts.empty());
    CHECK(named(testIds::gainController) == scan.plugins.end());
}

#if defined(__APPLE__)
TEST_CASE("A worker never shows in the Dock")
{
    Scan scan;
    REQUIRE(scan.scan(scan.fake(R"({"plugins": [{"id": "alpha", "name": "Alpha"}]})")));
    REQUIRE(scan.finished());
    CHECK_FALSE(showsInDock(scan.worker->processId()));
}
#endif
