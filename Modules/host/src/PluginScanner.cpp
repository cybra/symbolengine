#include "symbolengine/host/PluginScanner.h"

#include <symbolengine/core/Paths.h>
#include <symbolengine/core/System.h>

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <utility>

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#endif

namespace symbolengine
{

namespace
{

// The step a worker's failure comes at when it said nothing of the request: the worker's failure, never a plugin's.
constexpr std::string_view beforeAnything = "starting the scan";

// How many times in a row a worker may fail to start or answer before the scan stops.
constexpr int startsAllowed = 3;

// A folder no walk enters: a volume's root, a drive's mount point, or the user's home.
bool forbidden(const std::filesystem::path& folder)
{
    auto normal = folder.lexically_normal();
    if (!normal.has_filename())
        normal = normal.parent_path();
    std::error_code error;
    const auto home = paths::systemFolder(paths::Folder::home, error);
    if (normal == normal.root_path() || (!error && normal == home.lexically_normal()))
        return true;
    const auto parent = paths::toUtf8(normal.parent_path());
    return parent == "/Volumes" || parent == "/mnt" || parent == "/media" || parent == "/run/media" ||
           paths::toUtf8(normal.parent_path().parent_path()) == "/media" ||
           paths::toUtf8(normal.parent_path().parent_path()) == "/run/media";
}

// Whether a folder that isn't there is missing because its drive isn't connected, rather than because it was never
// made or was deleted: a Windows drive that isn't there, or a Mac or Linux volume that isn't mounted.
bool onAbsentDrive(const std::filesystem::path& folder)
{
    std::error_code error;
#if defined(_WIN32)
    return !std::filesystem::exists(folder.root_path(), error);
#else
    auto there = folder.lexically_normal();
    while (!there.empty() && there != there.root_path() && !std::filesystem::exists(there, error))
        there = there.parent_path();
    const auto at = paths::toUtf8(there);
    const auto above = paths::toUtf8(there.parent_path());
    return at == "/Volumes" || at == "/mnt" || at == "/media" || at == "/run/media" || above == "/media" ||
           above == "/run/media";
#endif
}

// A folder passed over in a walk: one hidden by its name, or on Windows by its attributes.
bool hidden(const std::filesystem::path& folder)
{
    if (paths::toUtf8(folder.filename()).starts_with("."))
        return true;
#if defined(_WIN32)
    const auto attributes = GetFileAttributesW(folder.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) != 0;
#else
    return false;
#endif
}

// The walk of one folder: every plugin file under it once by its real path, bundles taken whole and never entered,
// each folder entered once, links followed; and each folder under it that couldn't be read through, or holds something
// that couldn't be looked at, what the list has there being unknown rather than gone. False if the folder itself can't
// be read.
bool walkFolder(const std::filesystem::path& folder, const PluginFormat& format,
                std::set<std::filesystem::path>& entered, std::vector<std::filesystem::path>& found,
                std::map<std::filesystem::path, std::filesystem::path>& through,
                std::vector<std::filesystem::path>& unreadable, bool top)
{
    std::error_code error;
    std::filesystem::directory_iterator entries(folder, error);
    std::vector<std::filesystem::path> inside;

    // An error along the way ends the walk through it, leaving it at its end.
    if (!error)
        for (; entries != std::filesystem::directory_iterator(); entries.increment(error))
            inside.push_back(entries->path());

    if (error)
    {
        if (top)
            return false;
        unreadable.push_back(folder);
    }

    std::sort(inside.begin(), inside.end());

    for (const auto& path : inside)
    {
        std::error_code failed;
        const auto real = std::filesystem::canonical(path, failed);
        if (failed)
        {
            // A link to nothing, or round to itself, is nothing to scan; anything else may be a plugin not looked at.
            if (failed != std::errc::no_such_file_or_directory && failed != std::errc::too_many_symbolic_link_levels &&
                failed != std::errc::not_a_directory)
                unreadable.push_back(folder);
            continue;
        }
        if (format.isPluginFile(path))
        {
            found.push_back(real);
            if (real != path)
                through.emplace(real, path);
        }
        else if (std::filesystem::is_directory(real, failed) && !hidden(path) && !forbidden(real) &&
                 entered.insert(real).second)
            walkFolder(real, format, entered, found, through, unreadable, false);
    }
    return true;
}

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

    // When the list was last saved.
    std::chrono::steady_clock::time_point saved{};

    // The file in hand: what has been found in it so far, whether the request under way began in a worker that had
    // scanned no file before, and whether it was tried again in a fresh worker after failing in a used one.
    PluginFile record;
    bool inHand = false;
    bool asking = false;
    bool fresh = false;
    bool retried = false;

    Inner(PluginList& into, Options from) : list(into), options(std::move(from)) {}

    void save()
    {
        std::error_code failed;
        list.save(failed);
        saved = std::chrono::steady_clock::now();
        if (failed && owner != nullptr && owner->onSaveFailed)
            owner->onSaveFailed(failed);
    }

    // Records a file, saving the list once the time set has passed since it was last saved.
    void recordFile(const PluginFile& file)
    {
        list.record(file);
        if (std::chrono::steady_clock::now() - saved >= options.saveEvery)
            save();
    }

    void post(std::function<void(Inner&)> what)
    {
        options.worker.post([weak = weak_from_this(), what = std::move(what)] {
            if (const auto self = weak.lock())
                what(*self);
        });
    }

    // Lets a worker go after the current call has returned, as it may be one of the worker's own callbacks, and hears
    // nothing more from it.
    void retire()
    {
        if (worker == nullptr)
            return;
        ++workerNumber;
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
        record.through = file.through;
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
        worker->onAsking = mine([](Inner& self, bool now) { self.ask(now); });
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
        recordFile(record);
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

        if (plugin && failure.kind != PluginFailure::Kind::codeSignature)
        {
            record.setAside.push_back({*plugin, failure});
            return post([](Inner& self) { self.request(); });
        }

        // The file's code at fault, whichever of its plugins the system stopped it in, or the file itself.
        if (failure.kind == PluginFailure::Kind::codeSignature)
        {
            record.plugins.clear();
            record.setAside.clear();
            record.outcome = PluginFile::Outcome::changedAfterSigning;
        }
        else
            record.outcome = PluginFile::Outcome::setAside;
        record.failure = failure;
        record.complete = true;
        recordFile(record);
        inHand = false;
        ++index;
        post([](Inner& self) { self.next(); });
    }

    void finish(PluginScanEnd end)
    {
        scanning = false;
        inHand = false;
        retire();
        list.setScanUnderway(false);
        save();
        post([end](Inner& self) {
            if (self.owner != nullptr && self.owner->onFinished)
                self.owner->onFinished(end);
        });
    }

    // Tells the program whether the plugin in hand is asking something.
    void ask(bool now)
    {
        if (std::exchange(asking, now) == now)
            return;
        post([now, file = record.path](Inner& self) {
            if (self.owner != nullptr && self.owner->onAsking)
                self.owner->onAsking(file, now);
        });
    }

    // Records the file in hand as cut short, what was found of it let go, so it is scanned again.
    void leaveInHand()
    {
        if (!inHand)
            return;
        auto still = list.file(record.path).value_or(PluginFile{});
        if (still.path.empty())
        {
            still = record;
            still.plugins.clear();
            still.setAside.clear();
        }
        still.complete = false;
        list.record(still);
        inHand = false;
    }

    // Passes over the file in hand, its worker let go, and carries on with the next.
    void skip()
    {
        if (!scanning || !inHand)
            return;
        ask(false);
        retire();
        leaveInHand();
        ++index;
        post([](Inner& self) { self.next(); });
    }

    // Ends the scan with the file in hand left to be scanned again; a scan the program quits during is saved as still
    // under way.
    void stop(bool tell)
    {
        if (!scanning)
            return;
        ask(false);
        leaveInHand();
        if (tell)
            return finish({true, {}});
        scanning = false;
        inHand = false;
        save();
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

PluginWalk PluginScanner::walk(const std::vector<PluginScanFolder>& folders, PluginList& list)
{
    PluginWalk walked;

    // Each file found, with the folder it was found in, in the scan's order once sorted.
    struct Found
    {
        int last = 0;
        std::size_t folder = 0;
        PluginScanFile file;
    };
    std::vector<Found> found;
    std::set<std::filesystem::path> files;

    for (std::size_t index = 0; index < folders.size(); ++index)
    {
        const auto& [format, folder] = folders[index];
        if (format == nullptr)
            continue;
        if (forbidden(folder.path))
        {
            walked.refused.push_back(paths::toUtf8(folder.path) +
                                     " is a volume or a home folder, which a scan never walks.");
            continue;
        }

        // Gone only where the system says so. Where it won't say, as under a folder it won't enter, nothing is known.
        std::error_code error;
        const auto there = std::filesystem::exists(folder.path, error);
        if (error)
        {
            list.folderUnreadable(folder.path);
            continue;
        }
        if (!there)
        {
            const auto where = std::filesystem::weakly_canonical(folder.path, error);
            if (error)
                list.folderUnreadable(folder.path);
            else if (onAbsentDrive(folder.path))
                list.folderUnreadable(where);
            else
                list.folderRead(format->name(), where, {});
            continue;
        }

        const auto real = std::filesystem::canonical(folder.path, error);
        if (!error && forbidden(real))
        {
            walked.refused.push_back(paths::toUtf8(folder.path) +
                                     " is a volume or a home folder, which a scan never walks.");
            continue;
        }
        std::set<std::filesystem::path> entered{real};
        std::vector<std::filesystem::path> here, unreadable;
        std::map<std::filesystem::path, std::filesystem::path> through;
        if (error || !walkFolder(real, *format, entered, here, through, unreadable, true))
        {
            list.folderUnreadable(error ? folder.path : real);
            continue;
        }
        list.folderRead(format->name(), real, here, unreadable);

        // Each file once, whichever links and folders led to it.
        for (const auto& path : here)
        {
            if (!files.insert(path).second)
                continue;
            PluginScanFile file{format->name(), path, {}, folder.user, {}};
            if (const auto link = through.find(path); link != through.end())
            {
                file.through = link->second;
                list.reachedThrough(path, link->second);
            }
            PluginError problem;
            const auto facts = format->examine(path, problem);
            if (facts)
            {
                file.stamp = facts->stamp;
                file.architectures = facts->architectures;
            }

            // What can't be read, or isn't built for anything that can run here, is listed as that without a worker.
            const bool foreign =
                facts && !file.architectures.empty() &&
                std::none_of(file.architectures.begin(), file.architectures.end(),
                             [&](const std::string& architecture) { return format->runs(architecture); });
            if (!facts || foreign)
            {
                if (list.needsScan(path, file.stamp))
                {
                    PluginFile listed{path,
                                      file.format,
                                      folder.user,
                                      file.stamp,
                                      file.architectures,
                                      foreign ? PluginFile::Outcome::otherArchitecture : PluginFile::Outcome::setAside};
                    if (!facts)
                        listed.failure =
                            PluginFailure{PluginFailure::Kind::refused, "reading the file", 0, problem.message};
                    list.record(listed);
                }
                continue;
            }

            // Those that held many plugins go last, and those set aside before after them.
            int last = 0;
            if (const auto before = list.file(path))
            {
                if (before->outcome == PluginFile::Outcome::setAside || !before->setAside.empty())
                    last = 2;
                else if (before->plugins.size() > 1)
                    last = 1;
            }
            found.push_back({last, index, std::move(file)});
        }
    }

    std::stable_sort(found.begin(), found.end(), [](const Found& a, const Found& b) {
        return std::tie(a.last, a.folder, a.file.path) < std::tie(b.last, b.folder, b.file.path);
    });
    for (auto& one : found)
        walked.files.push_back(std::move(one.file));
    return walked;
}

bool PluginScanner::scan(std::vector<PluginScanFile> files)
{
    if (inner->scanning)
        return false;
    inner->files = std::move(files);
    inner->index = 0;
    inner->failedStarts = 0;
    inner->scanning = true;
    inner->list.setScanUnderway(true);
    inner->save();
    inner->post([](Inner& self) { self.next(); });
    return true;
}

void PluginScanner::skip()
{
    inner->skip();
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
