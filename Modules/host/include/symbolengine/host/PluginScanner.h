#pragma once

#include "PluginList.h"
#include "PluginScanWorker.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace symbolengine
{

/** A file for a scan to look at, as the folder walk finds it: its format, where it lies, its stamp, whether it lies in
    the user's own folder or one the user added, and the architectures its binaries are built for. */
struct PluginScanFile
{
    std::string format{};
    std::filesystem::path path{};
    PluginStamp stamp{};
    bool userFolder = false;
    std::vector<std::string> architectures{};

    /** Where in the folder it was reached through a link, when its path is elsewhere. */
    std::filesystem::path through{};
};

/** A folder for a walk: one of a format's own or one the user added, with the format whose files it holds. */
struct PluginScanFolder
{
    PluginFormat* format = nullptr;
    PluginFolder folder{};
};

/** What a walk found: the files for a scan, in the scan's order, and the folders it wouldn't walk, with why. */
struct PluginWalk
{
    std::vector<PluginScanFile> files{};
    std::vector<std::string> refused{};
};

/** How a scan ended: through every file, stopped, or unable to start or reach a worker three times in a row, with the
    system's reason and the worker's file, so security software can be told to allow it. */
struct PluginScanEnd
{
    bool stopped = false;
    std::string problem{};
};

/** A scan of plugin files in worker processes, one file and one worker at a time, what each worker finds recorded in
    the list as it comes.

    A plugin that crashes its worker, hangs past the limit or ends it without answering is set aside, and the scan goes
    on with a fresh worker, past it in a file holding many. A failure in a worker that scanned a file before is tried
    once more in a fresh one before anything is set aside, so damage one plugin leaves in a worker is never blamed on
    the next. A worker that can't be started, or ends before saying anything of a request, blames no plugin, and three
    in a row stop the scan. A worker is replaced after a set number of files or once it uses more memory than set.
    Stopping fails nothing: what was found of the file in hand is let go and the file recorded as cut short, to be
    scanned again next time. The list is saved as the scan begins, at least as often as set while it records files,
    and as it ends, with whether a scan is under way, so one the program quit during carries on. Everything is on the
    program's main thread, where the list must outlive the scanner. */
class PluginScanner
{
public:
    struct Options
    {
        /** How each worker is started and followed; its post is the program's main thread's. */
        PluginScanWorker::Options worker;

        /** How effects are set up as they are scanned: at the rate the program's device last ran at. */
        PluginSetup setup{};

        /** How many files a worker scans, and how much memory it may come to use, before it is replaced. */
        int filesPerWorker = 50;
        std::int64_t workerMemory = std::int64_t(2) * 1024 * 1024 * 1024;

        /** How often the list is saved while files are recorded; it is also saved as a scan begins and ends. */
        std::chrono::milliseconds saveEvery{10'000};
    };

    /** Walks the folders for the plugin files in them, each folder and file once by its real path, a bundle taken
        whole, links followed, hidden folders passed over, and never a volume or a home folder. Records in the list the
        files gone from a folder it read, which a folder that isn't there is read as, and those kept in a folder it
        couldn't read or whose drive isn't connected, and the files built only for other architectures, which no worker
        is given. Gives the files for a scan in
        its order: by folder and name, those that held many plugins and then those set aside before last. */
    static PluginWalk walk(const std::vector<PluginScanFolder>& folders, PluginList& list);

    PluginScanner(PluginList& list, Options options);

    /** Stops a scan under way, as stop() does. */
    ~PluginScanner();

    PluginScanner(const PluginScanner&) = delete;
    PluginScanner& operator=(const PluginScanner&) = delete;

    /** Told of the file the scan is on and the step its worker is on, and once, when the scan ends, how it ended. */
    std::function<void(const std::filesystem::path& file, const std::string& step)> onProgress;
    std::function<void(const PluginScanEnd& end)> onFinished;

    /** The list couldn't be saved, as its folder is read-only or the disk full. What was found so far is kept in memory and
        saving is tried again later; this is for saying so. */
    std::function<void(const std::error_code& why)> onSaveFailed;

    /** Told when a plugin being scanned shows a window, as a license question does, and when it is gone: the program
        says the plugin is asking something, with Skip, while the limit waits. */
    std::function<void(const std::filesystem::path& file, bool asking)> onAsking;

    /** Scans the files the list says need it, in the order given; false while a scan is under way. */
    bool scan(std::vector<PluginScanFile> files);

    /** Stops the scan under way: its worker ended and the file in hand recorded as cut short. */
    void stop();

    /** Skips the file in hand, as for a plugin asking something the user won't answer now: its worker ended, the file
        recorded as cut short, to be scanned again next time and never set aside, and the scan carried on. */
    void skip();

    bool isScanning() const;

    /** How many workers this scanner has started, and how many of them it had to kill, past the limit or not ending
        by themselves once let go. */
    int workersStarted() const;
    int workersKilled() const;

    struct Inner;

private:
    std::shared_ptr<Inner> inner;
};

}
