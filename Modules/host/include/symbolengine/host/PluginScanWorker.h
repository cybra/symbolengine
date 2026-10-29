#pragma once

#include "PluginFormat.h"
#include "PluginList.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <symbolengine/core/ChildProcess.h>

namespace symbolengine
{

/** What a program asks a scan worker to look at: a file of one of the worker's formats, its effects set up as the
    setup says, and those of its plugins already answered skipped, as when a scan carries on past one that crashed. */
struct PluginScanRequest
{
    std::string format{};
    std::filesystem::path file{};
    PluginSetup setup{};
    std::vector<PluginIdentity> skip{};
};

/** A worker process that loads plugin files for a scan, so a plugin that crashes or hangs takes down only it: a copy of
    the program started in worker mode, and the program's link to it.

    In the worker, main asks isWorker() before anything else of the program runs, its settings, logging, crash
    reporting, single-instance handoff and windows included, makes its formats on the worker's main thread and calls
    work(), which never returns:

        int main(int argc, char** argv)
        {
            if (PluginScanWorker::isWorker(argc, argv))
            {
                Vst3Format vst3({"My Program", PluginScanWorker::post, PluginScanWorker::onMainThread});
                PluginScanWorker::work({&vst3});
            }
            ...
        }

    The worker loads plugins on its main thread with its run loop turning, as plugins need, on a Mac as an accessory
    that never shows in the Dock. Its effects are each made, offered stereo, set up, played blocks of noise and silence
    and a short block and taken down, so what passes is what the program will play; other kinds are answered as they
    declare themselves, never made. It says each step as it begins it, writing it too to a note its crash handler adds
    the signal and a backtrace to, which comes with a crash's failure; answers each plugin as it finishes and the file
    once it is done; comes to the front when a window of its own appears, as a plugin's license question does; and
    ends the moment its link to the program is lost, from the link's own thread, running nothing
    more of anything it loaded; the link beats every second both ways from threads of its own.

    In the program, start() starts a worker and scan() asks it for one file at a time. What it says is checked before
    use and told to the callbacks on the program's main thread. A worker that makes no progress within the limit, beginning
    no step and answering nothing, is killed and confirmed gone, and one that ends before answering has failed, however
    it ended. */
class PluginScanWorker
{
public:
    /** In the program as it starts: whether it was started as a scan worker, after which it is one and nothing else
        of the program is to run. Its main thread is the thread asking. */
    static bool isWorker(int argc, const char* const* argv);

    /** In a worker: its main thread, for its formats. post runs a function there after those posted before it, from
        any thread; onMainThread says whether the caller is on it. */
    static void post(std::function<void()> function);
    static bool onMainThread();

    /** In a worker: answers the program's requests with the formats until the program goes, on the main thread. */
    [[noreturn]] static void work(std::vector<PluginFormat*> formats);

    struct Options
    {
        /** The program to start as the worker: the program's own executable (paths::executableFile()). */
        std::filesystem::path program;

        /** Variables set in the worker's environment, over those it inherits. */
        std::vector<std::pair<std::string, std::string>> environment;

        /** A file the worker's output is added to, for finding what went wrong; with none it is thrown away. */
        std::filesystem::path outputFile;

        /** How long the worker may go without beginning a new step or answering before it is killed. */
        std::chrono::milliseconds limit{60'000};

        /** Runs a function on the program's main thread, where the callbacks are told: ui::messageLoop::callAsync in
            a program with one. */
        std::function<void(std::function<void()>)> post;
    };

    /** Starts a worker, or gives nothing with the error set, which blames no plugin. */
    static std::unique_ptr<PluginScanWorker> start(Options options, std::error_code& error);

    /** Ends the worker as close() does, waiting 2 seconds for it. */
    ~PluginScanWorker();

    PluginScanWorker(const PluginScanWorker&) = delete;
    PluginScanWorker& operator=(const PluginScanWorker&) = delete;

    /** Told on the program's main thread, for the request under way: the step the worker is on as it changes, each
        plugin it found and each it set aside, and the file's outcome once it is done, with why for one set aside,
        after which it takes the next request. */
    std::function<void(const std::string& step)> onProgress;
    std::function<void(const PluginDescription& plugin)> onPlugin;
    std::function<void(const PluginSetAside& plugin)> onSetAside;
    std::function<void(PluginFile::Outcome outcome, const std::optional<PluginFailure>& why)> onDone;

    /** Told when a window of the worker's own appears, as a plugin's license question does, and when it is gone. The
        worker puts its windows above every other program's, the program hands it the keyboard if the program is in
        front, and the limit is paused while it shows, as the user may be answering. */
    std::function<void(bool asking)> onAsking;

    /** Told once, on the program's main thread, when the worker crashed, made no progress within the limit or ended
        before answering the request under way: it is gone, and the failure says how, at the step it was on, with the
        plugin it had in hand, if the step was one plugin's. A failure at the step "starting the scan" came before the
        worker said anything of the request, so it was the worker's, never a plugin's. */
    std::function<void(const PluginFailure& failure, const std::optional<PluginDescription>& inHand)> onFailed;

    /** Asks the worker to scan a file, giving false while a request is under way or once it has gone. */
    bool scan(const PluginScanRequest& request);

    /** Whether a request is under way, and whether the worker has gone. */
    bool isBusy() const;
    bool hasEnded() const;

    /** The worker's process ID, as the system shows it, and the memory it uses now, in bytes. */
    std::int64_t processId() const;
    std::int64_t memoryInUse() const;

    /** Closes the link, so the worker ends by itself, and waits up to the limit for it, killing it after; gives how it
        ended. Nothing more is told. */
    ProcessEnding close(std::chrono::milliseconds limit = std::chrono::seconds(2));

    struct Shared;

private:
    explicit PluginScanWorker(std::shared_ptr<Shared> shared);

    std::shared_ptr<Shared> shared;
};

}
