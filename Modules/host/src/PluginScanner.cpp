#include "symbolengine/host/PluginScanner.h"

#include <symbolengine/core/Paths.h>

#include <optional>
#include <utility>

namespace symbolengine
{

namespace
{

// The step a worker's failure comes at when it said nothing of the request: the worker's failure, never a plugin's.
constexpr std::string_view beforeAnything = "starting the scan";

// How many times in a row a worker may fail to start or answer before the scan stops.
constexpr int startsAllowed = 3;

}

struct PluginScanner::Inner : std::enable_shared_from_this<PluginScanner::Inner>
{
    PluginList& list;
    Options options;

    // The program's object, while it wants to be told anything.
    PluginScanner* owner = nullptr;

    bool scanning = false;
    std::vector<PluginScanFile> files;
    std::size_t index = 0;

    // The worker, which callbacks of an earlier one are told apart from by its number, how many files it has finished,
    // and whether a plugin crashed in it with the host catching the fault, after which it can't be trusted.
    std::unique_ptr<PluginScanWorker> worker;
    int workerNumber = 0;
    int workerFiles = 0;
    bool workerTrusted = true;
    int started = 0;
    int killed = 0;
    int failedStarts = 0;

    // The file in hand: what has been found in it so far, whether the request under way began in a worker that had
    // scanned no file before, and whether it was tried again in a fresh worker after failing in a used one.
    PluginFile record;
    bool inHand = false;
    bool fresh = false;
    bool retried = false;

    Inner(PluginList& into, Options from) : list(into), options(std::move(from)) {}

    void post(std::function<void(Inner&)> what)
    {
        options.worker.post([weak = weak_from_this(), what = std::move(what)] {
            if (const auto self = weak.lock())
                what(*self);
        });
    }

    // Lets a worker go after the current call has returned, as it may be one of the worker's own callbacks.
    void retire()
    {
        if (worker == nullptr)
            return;
        std::shared_ptr<PluginScanWorker> going(std::move(worker));
        post([going](Inner& self) {
            if (going->close().killed)
                ++self.killed;
        });
    }

    void next()
    {
        if (!scanning)
            return;
        while (index < files.size() && !list.needsScan(files[index].path, files[index].stamp))
            ++index;
        if (index == files.size())
            return finish({});

        const auto& file = files[index];
        record = PluginFile{};
        record.path = file.path;
        record.format = file.format;
        record.userFolder = file.userFolder;
        record.stamp = file.stamp;
        record.architectures = file.architectures;
        record.complete = false;
        inHand = true;
        retried = false;
        request();
    }

    // Asks a worker for the file in hand, with what was already answered of it skipped, starting a fresh worker where
    // the last has ended, scanned its share of files, grown too large or can't be trusted.
    void request()
    {
        if (!scanning)
            return;
        if (worker != nullptr && (worker->hasEnded() || !workerTrusted || workerFiles >= options.filesPerWorker ||
                                  worker->memoryInUse() > options.workerMemory))
            retire();
        if (worker == nullptr && !startWorker())
            return;

        PluginScanRequest asked{record.format, record.path, options.setup, {}};
        for (const auto& plugin : record.plugins)
            asked.skip.push_back(plugin.identity);
        for (const auto& plugin : record.setAside)
            asked.skip.push_back(plugin.plugin.identity);
        fresh = workerFiles == 0;
        if (!worker->scan(asked))
        {
            retire();
            failedStart("The scan's worker couldn't be given a file.");
        }
    }

    bool startWorker()
    {
        std::error_code error;
        worker = PluginScanWorker::start(options.worker, error);
        if (worker == nullptr)
        {
            failedStart("The scan's worker " + paths::toUtf8(options.worker.program) +
                        " can't be started: " + error.message());
            return false;
        }

        ++started;
        workerFiles = 0;
        workerTrusted = true;
        const int number = ++workerNumber;
        const auto mine = [weak = weak_from_this(), number](auto what) {
            return [weak, number, what](const auto&... said) {
                if (const auto self = weak.lock(); self && self->scanning && self->workerNumber == number)
                    what(*self, said...);
            };
        };
        worker->onProgress = mine([](Inner& self, const std::string& step) {
            self.failedStarts = 0;
            if (self.owner != nullptr && self.owner->onProgress)
                self.owner->onProgress(self.record.path, step);
        });
        worker->onPlugin = mine([](Inner& self, const PluginDescription& plugin) {
            self.failedStarts = 0;
            self.record.plugins.push_back(plugin);
        });
        worker->onSetAside = mine([](Inner& self, const PluginSetAside& plugin) {
            self.record.setAside.push_back(plugin);
            if (plugin.failure.kind == PluginFailure::Kind::crashed)
                self.workerTrusted = false;
        });
        worker->onDone = mine([](Inner& self, PluginFile::Outcome outcome, const std::optional<PluginFailure>& why) {
            self.done(outcome, why);
        });
        worker->onFailed = mine([](Inner& self, const PluginFailure& failure,
                                   const std::optional<PluginDescription>& plugin) { self.failed(failure, plugin); });
        return true;
    }

    void failedStart(const std::string& problem)
    {
        if (++failedStarts >= startsAllowed)
            return finish({false, problem});
        post([](Inner& self) { self.request(); });
    }

    void done(PluginFile::Outcome outcome, const std::optional<PluginFailure>& why)
    {
        record.outcome = outcome;
        record.failure = why;
        record.complete = true;
        list.record(record);
        inHand = false;
        ++workerFiles;
        ++index;
        post([](Inner& self) { self.next(); });
    }

    // The worker crashed, hung or ended before answering: a failure in a worker that scanned a file before is tried
    // once more in a fresh one; otherwise the plugin in hand is set aside and the file carried on past it, or the file
    // itself is set aside when no plugin was in hand.
    void failed(const PluginFailure& failure, const std::optional<PluginDescription>& plugin)
    {
        retire();
        if (failure.step == beforeAnything)
            return failedStart("The scan's worker " + paths::toUtf8(options.worker.program) +
                               " ended before answering: " + failure.detail);
        failedStarts = 0;

        if (!fresh && !retried)
        {
            retried = true;
            return post([](Inner& self) { self.request(); });
        }

        if (plugin)
        {
            record.setAside.push_back({*plugin, failure});
            return post([](Inner& self) { self.request(); });
        }

        record.outcome = PluginFile::Outcome::setAside;
        record.failure = failure;
        record.complete = true;
        list.record(record);
        inHand = false;
        ++index;
        post([](Inner& self) { self.next(); });
    }

    void finish(PluginScanEnd end)
    {
        scanning = false;
        inHand = false;
        retire();
        post([end](Inner& self) {
            if (self.owner != nullptr && self.owner->onFinished)
                self.owner->onFinished(end);
        });
    }

    // Ends the scan with the file in hand recorded as cut short, so it is scanned again.
    void stop(bool tell)
    {
        if (!scanning)
            return;
        if (inHand)
            list.record(record);
        if (tell)
            return finish({true, {}});
        scanning = false;
        inHand = false;
    }
};

PluginScanner::PluginScanner(PluginList& list, Options options)
    : inner(std::make_shared<Inner>(list, std::move(options)))
{
    inner->owner = this;
}

PluginScanner::~PluginScanner()
{
    inner->owner = nullptr;
    inner->stop(false);
    if (inner->worker != nullptr)
        inner->worker->close();
}

bool PluginScanner::scan(std::vector<PluginScanFile> files)
{
    if (inner->scanning)
        return false;
    inner->files = std::move(files);
    inner->index = 0;
    inner->failedStarts = 0;
    inner->scanning = true;
    inner->post([](Inner& self) { self.next(); });
    return true;
}

void PluginScanner::stop()
{
    inner->stop(true);
}

bool PluginScanner::isScanning() const
{
    return inner->scanning;
}

int PluginScanner::workersStarted() const
{
    return inner->started;
}

int PluginScanner::workersKilled() const
{
    return inner->killed;
}

}
