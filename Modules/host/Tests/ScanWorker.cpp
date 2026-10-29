// The program the scan worker tests start as a worker: a fake format whose files say what each plugin in them does, and
// the VST3 format for the test module. Its environment can make it a worker that answers wrongly, or a program with a
// worker of its own that the tests kill or let quit.

#include <symbolengine/core/ChildProcess.h>
#include <symbolengine/core/Files.h>
#include <symbolengine/core/Json.h>
#include <symbolengine/core/Paths.h>
#include <symbolengine/core/System.h>
#include <symbolengine/host/FakePluginFormat.h>
#include <symbolengine/host/PluginScanWorker.h>
#include <symbolengine/host/Vst3Format.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace symbolengine;
using namespace std::chrono_literals;

namespace
{

[[noreturn]] void hang()
{
    for (;;)
        std::this_thread::sleep_for(1s);
}

// What a fake plugin does inside one of its calls, as its file says: hangs, on its own thread or on one it waits for;
// crashes by writing through a null pointer; ends its process as if all were well; or takes its time.
void misbehave(const Value& plugin)
{
    const auto does = plugin["does"].stringOr("");
    if (does == "hang")
        hang();
    if (does == "hangOnThread")
        std::thread(hang).join();
    if (does == "crash")
    {
        volatile int* nowhere = nullptr;
        *nowhere = 1;
    }
    if (does == "exit")
        std::exit(0);
    if (does == "sleep")
        std::this_thread::sleep_for(std::chrono::milliseconds(plugin["ms"].intOr(0)));
}

// The plugins a fake file holds, each scripted as it says, or the error it says its code fails to load with.
std::vector<PluginDescription> fakeFile(FakePluginFormat& fake, const std::filesystem::path& file, PluginError& error)
{
    std::error_code readError;
    const auto contents = json::tryParse(files::read(file, readError));
    if (readError || !contents)
    {
        error = {"The fake file can't be read.", "read", 0};
        return {};
    }
    if (const auto* why = (*contents)["error"].ifString())
    {
        error = {*why, "load", 0};
        return {};
    }

    std::vector<PluginDescription> plugins;
    for (const auto& plugin : (*contents)["plugins"].asArray())
    {
        auto description = FakePluginFormat::describe(plugin["id"].asString());
        description.name = plugin["name"].stringOr(description.name);
        description.maker = "Fake Maker";
        description.sdk = "Fake 1";
        const auto kind = plugin["kind"].stringOr("effect");
        description.kind = kind == "instrument" ? PluginKind::instrument
                           : kind == "other"    ? PluginKind::other
                                                : PluginKind::effect;

        FakePluginFormat::Script script;
        if (const auto* takes = plugin["takes"].ifArray())
        {
            script.takes.clear();
            for (const auto& layout : *takes)
                script.takes.push_back({static_cast<int>(layout[0].asInt()), static_cast<int>(layout[1].asInt())});
        }
        script.failToMake = plugin["failToMake"].boolOr(false);
        script.failToActivate = plugin["failToActivate"].boolOr(false);
        script.inside = [plugin, at = plugin["at"].stringOr("arrange")](std::string_view call) {
            if (call == at)
                misbehave(plugin);

            // Time taken in each call it names, as {"arrange": 300}.
            if (const auto ms = plugin["sleeps"][call].intOr(0); ms > 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        };
        fake.script(description.identity.id, script);
        plugins.push_back(description);
    }
    return plugins;
}

// A worker that answers the first request wrongly in the way its mode says, speaking to the program itself.
[[noreturn]] void answerWrongly(const std::string& mode)
{
    std::error_code error;
    auto link = ProcessLink::toParent(error);
    if (link == nullptr)
        std::exit(90);
    link->onLost([] { system::endNow(7); });

    std::mutex lock;
    std::condition_variable arrived;
    std::int64_t request = -1;
    link->onMessage([&](std::string message) {
        const std::lock_guard held(lock);
        request = json::parse(message)["request"].asInt();
        arrived.notify_all();
    });
    link->start();
    {
        std::unique_lock held(lock);
        arrived.wait(held, [&] { return request >= 0; });
    }

    const auto number = std::to_string(request);
    const std::string plugin = R"({"id":"wrong","name":"Wrong","maker":"Fake Maker","version":"1.0","kind":"effect",)"
                               R"("layouts":[[2,2]],"editor":false,"sdk":"Fake 1"})";
    if (mode == "wrongNumber")
    {
        const auto other = std::to_string(request + 1);
        link->send(R"({"request":)" + other + R"(,"plugin":)" + plugin + "}");
        link->send(R"({"request":)" + other + R"(,"done":"found"})");
        hang();
    }
    if (mode == "cutShort")
    {
        const std::string whole = R"({"request":)" + number + R"(,"plugin":)" + plugin + "}";
        link->send(whole.substr(0, whole.size() / 2));
    }
    if (mode == "empty")
        link->send("");
    if (mode == "notAPlugin")
    {
        link->send(R"({"request":)" + number + R"(,"plugin":{"name":"No identity"}})");
        link->send(R"({"request":)" + number + R"(,"plugin":{"id":7}})");
        link->send(R"({"request":)" + number + R"(,"plugin":[1,2]})");
        link->send(R"({"request":)" + number + R"(,"done":"maybe"})");
        link->send(R"([1,2,3])");
        link->send(R"({"request":)" + number + R"(,"plugin":)" + plugin + "}");
        link->send(R"({"request":)" + number + R"(,"done":"found"})");
        hang();
    }
    if (mode == "twice")
    {
        link->send(R"({"request":)" + number + R"(,"plugin":)" + plugin + "}");
        link->send(R"({"request":)" + number + R"(,"plugin":)" + plugin + "}");
        link->send(R"({"request":)" + number + R"(,"done":"found"})");
        link->send(R"({"request":)" + number + R"(,"done":"nothing"})");
        link->send(R"({"request":)" + number + R"(,"plugin":)" + plugin + "}");
        hang();
    }
    if (mode == "answersThenEnds")
    {
        // Thousands of answers and the file's, and gone the moment they are sent.
        for (int i = 0; i < 5000; ++i)
            link->send(R"({"request":)" + number + R"(,"plugin":{"id":"p)" + std::to_string(i) +
                       R"(","name":"Many","kind":"instrument"}})");
        link->send(R"({"request":)" + number + R"(,"done":"found"})");
        system::endNow(0);
    }
    if (mode == "progressOnly")
        link->send(R"({"request":)" + number + R"(,"step":"loading the code","count":1})");

    // Long enough for what was sent to arrive, then gone without answering.
    std::this_thread::sleep_for(300ms);
    system::endNow(0);
}

}

int main(int argc, char** argv)
{
    if (PluginScanWorker::isWorker(argc, argv))
    {
        if (const auto mode = system::environmentVariable("SYMBOLENGINE_TEST_WORKER_MODE"))
            answerWrongly(*mode);

        FakePluginFormat fake(PluginScanWorker::onMainThread, PluginScanWorker::post);
        fake.files(
            [&fake](const std::filesystem::path& file, PluginError& error) { return fakeFile(fake, file, error); });
        Vst3Format vst3({"SymbolEngine tests", PluginScanWorker::post, PluginScanWorker::onMainThread});
        PluginScanWorker::work({&fake, &vst3});
    }

    // A program with a worker of its own, which writes the worker's process ID to a file and then waits to be killed,
    // or, once the file its third argument names is there, quits without letting the worker go.
    if (argc >= 3)
    {
        std::error_code error;
        auto worker = PluginScanWorker::start(
            {paths::executableFile(), {}, {}, std::chrono::seconds(60), [](std::function<void()>) {}}, error);
        if (worker == nullptr)
            return 91;
        files::writeSafely(paths::fromUtf8(argv[2]), std::to_string(worker->processId()));
        if (std::string(argv[1]) == "quits" && argc >= 4)
        {
            while (!std::filesystem::exists(paths::fromUtf8(argv[3])))
                std::this_thread::sleep_for(10ms);
            std::exit(0);
        }
        hang();
    }
    return 64;
}
