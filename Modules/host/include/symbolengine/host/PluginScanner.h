#pragma once

#include "PluginList.h"
#include "PluginScanWorker.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace symbolengine
{

/** A file for a scan to look at, as the folder walk finds it: its format, where it lies, its stamp, whether it lies in
    a folder the user added, and the architectures its binary is built for. */
struct PluginScanFile
{
    std::string format{};
    std::filesystem::path path{};
    PluginStamp stamp{};
    bool userFolder = false;
    std::vector<std::string> architectures{};
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
    Stopping fails nothing: the file in hand is recorded as cut short, to be scanned again next time. Everything is on
    the program's main thread. */
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
    };

    PluginScanner(PluginList& list, Options options);

    /** Stops a scan under way, as stop() does. */
    ~PluginScanner();

    PluginScanner(const PluginScanner&) = delete;
    PluginScanner& operator=(const PluginScanner&) = delete;

    /** Told of the file the scan is on and the step its worker is on, and once, when the scan ends, how it ended. */
    std::function<void(const std::filesystem::path& file, const std::string& step)> onProgress;
    std::function<void(const PluginScanEnd& end)> onFinished;

    /** Scans the files the list says need it, in the order given; false while a scan is under way. */
    bool scan(std::vector<PluginScanFile> files);

    /** Stops the scan under way: its worker ended and the file in hand recorded as cut short. */
    void stop();

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
