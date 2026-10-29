#include "symbolengine/host/PluginScanWorker.h"

#include "PluginJson.h"
#include "ScanWorkerLoop.h"
#include "symbolengine/host/Plugin.h"

#include <symbolengine/core/Json.h>
#include <symbolengine/core/Paths.h>
#include <symbolengine/core/System.h>

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <set>
#include <thread>

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#else
    #include <pthread.h>
#endif

namespace symbolengine
{

namespace
{

using namespace pluginJson;
using Steady = std::chrono::steady_clock;

// The argument a worker is started with.
constexpr std::string_view workerArgument = "--symbolengine-scan-worker";

// Runs a function on a thread of its own with a stack of 8 MB, as plugins assume a main thread's, and waits for it.
void onLargeStack(const std::function<void()>& function)
{
    constexpr std::size_t stack = 8 * 1024 * 1024;
#if defined(_WIN32)
    const HANDLE thread = CreateThread(
        nullptr, stack,
        [](void* context) -> DWORD {
            (*static_cast<const std::function<void()>*>(context))();
            return 0;
        },
        const_cast<std::function<void()>*>(&function), STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (thread == nullptr)
        return function();
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
#else
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, stack);
    pthread_t thread;
    const auto started = pthread_create(
        &thread, &attributes,
        [](void* context) -> void* {
            (*static_cast<const std::function<void()>*>(context))();
            return nullptr;
        },
        const_cast<std::function<void()>*>(&function));
    pthread_attr_destroy(&attributes);
    if (started != 0)
        return function();
    pthread_join(thread, nullptr);
#endif
}

// The worker: takes the program's requests on its link's thread and scans each on the main thread, one after
// another, saying each step as it begins it.
class Worker
{
public:
    Worker(std::vector<PluginFormat*> those, ProcessLink& to)
        : formats(std::move(those)), link(to),
          host({scanWorkerLoop::post, scanWorkerLoop::onMainThread, std::chrono::milliseconds(2000),
                [](const std::string& what) { std::fprintf(stderr, "Scan worker: %s\n", what.c_str()); }})
    {
    }

    // A request from the program, checked before it is taken.
    void received(const std::string& message)
    {
        Request request;
        try
        {
            const auto value = json::parse(message);
            const auto& object = value.asObject();
            request.number = required(object, "request").asInt();
            request.format = required(object, "format").asString();
            request.file = paths::fromUtf8(required(object, "file").asString());
            request.setup.rate = required(object, "rate").asNumber();
            request.setup.maximumFrames = static_cast<int>(required(object, "frames").asInt());
            for (const auto& skipped : required(object, "skip").asArray())
                request.skip.insert(
                    {required(skipped.asObject(), "format").asString(), required(skipped.asObject(), "id").asString()});
        }
        catch (const std::exception& error)
        {
            std::fprintf(stderr, "Scan worker: a request that can't be read: %s\n", error.what());
            return;
        }

        {
            const std::lock_guard held(lock);
            waiting.push_back(std::move(request));
        }
        scanWorkerLoop::post([this] { startNext(); });
    }

private:
    struct Request
    {
        std::int64_t number = 0;
        std::string format;
        std::filesystem::path file;
        PluginSetup setup;
        std::set<PluginIdentity> skip;
    };

    std::vector<PluginFormat*> formats;
    ProcessLink& link;
    PluginHost host;

    // Shared with the link's thread.
    std::mutex lock;
    std::deque<Request> waiting;
    bool scanning = false;
    std::int64_t number = 0;
    std::string step;
    std::int64_t count = 0;

    // The main thread's: the request under way and the plugin it is on.
    Request current;
    PluginFormat* format = nullptr;
    std::vector<PluginDescription> plugins;
    std::size_t next = 0;
    std::shared_ptr<Plugin> plugin;

    void say(std::string now)
    {
        std::string message;
        {
            const std::lock_guard held(lock);
            step = std::move(now);
            ++count;
            message = json::write(Object{{"request", number}, {"step", step}, {"count", count}});
        }
        link.send(message);
    }

    void answer(Object message)
    {
        message.set("request", current.number);
        link.send(json::write(message));
    }

    std::string position() const
    {
        return "plugin " + std::to_string(next + 1) + " of " + std::to_string(plugins.size()) + ", " +
               plugins[next].name;
    }

    std::string currentStep()
    {
        const std::lock_guard held(lock);
        return step;
    }

    void startNext()
    {
        {
            const std::lock_guard held(lock);
            if (scanning || waiting.empty())
                return;
            current = std::move(waiting.front());
            waiting.pop_front();
            scanning = true;
            number = current.number;
        }

        say("loading the code");
        format = nullptr;
        for (auto* one : formats)
            if (one->name() == current.format)
                format = one;
        if (format == nullptr)
            return refuseFile("This worker has no format called " + current.format + ".");

        PluginError error;
        plugins = format->pluginsIn(current.file, error);
        if (error)
            return refuseFile(error.message);
        if (plugins.empty())
            return done(PluginFile::Outcome::nothing, std::nullopt);
        next = 0;
        nextPlugin();
    }

    void refuseFile(const std::string& why)
    {
        done(PluginFile::Outcome::setAside, PluginFailure{PluginFailure::Kind::refused, currentStep(), 0, why});
    }

    void done(PluginFile::Outcome outcome, const std::optional<PluginFailure>& why)
    {
        Object message{{"done", nameOf(outcome, outcomes)}};
        if (why)
            message.set("failure", writeFailure(*why));
        answer(std::move(message));
        plugins.clear();
        {
            const std::lock_guard held(lock);
            scanning = false;
        }
        scanWorkerLoop::post([this] { startNext(); });
    }

    void nextPlugin()
    {
        while (next < plugins.size() && current.skip.contains(plugins[next].identity))
            ++next;
        if (next == plugins.size())
            return done(PluginFile::Outcome::found, std::nullopt);

        // Only effects go in a chain, so only they are made; the rest are answered as they declare themselves.
        if (plugins[next].kind != PluginKind::effect)
        {
            say("reading " + position());
            found(plugins[next]);
            return following();
        }

        say("making " + position());
        plugin = host.make(*format, plugins[next]);
        plugin->onChange = [this, made = false](PluginChange change) mutable {
            if (change == PluginChange::made && !std::exchange(made, true))
                scanWorkerLoop::post([this] { check(); });
        };
    }

    // Made, or failed to be: set up, played and read as the program will play it.
    void check()
    {
        auto description = plugins[next];
        if (plugin->state() == Plugin::State::failed)
        {
            // One that gives no audio out is found as that, never set aside.
            if (plugin->problem().step == "arrange" && plugin->layout().outputs <= 0)
            {
                description.layouts = {plugin->layout()};
                found(description);
            }
            else
                refuse(plugin->problem().message);
            return following();
        }

        say("setting up " + position());
        if (!plugin->prepare(current.setup))
        {
            refuse(plugin->problem().message);
            return following();
        }

        say("playing " + position());
        play();
        if (plugin->state() == Plugin::State::failed)
        {
            refuse(plugin->problem().message);
            return following();
        }

        description.layouts = {plugin->layout()};
        plugin->release();
        found(description);
        following();
    }

    // Blocks of noise and of silence and a short block, on an audio thread of the worker's own.
    void play()
    {
        const auto frames = std::max(1, current.setup.maximumFrames);
        std::vector<float> left(static_cast<std::size_t>(frames)), right(static_cast<std::size_t>(frames));
        onLargeStack([&] {
            std::uint32_t noise = 12345;
            for (std::size_t i = 0; i < left.size(); ++i)
            {
                noise = noise * 1664525u + 1013904223u;
                left[i] = static_cast<float>(noise >> 8) / 16777216.0f * 0.5f - 0.25f;
                right[i] = -left[i];
            }
            float* channels[] = {left.data(), right.data()};
            plugin->process(channels, 2, frames);
            std::fill(left.begin(), left.end(), 0.0f);
            std::fill(right.begin(), right.end(), 0.0f);
            plugin->process(channels, 2, frames);
            plugin->process(channels, 2, std::max(1, frames / 3));
        });
    }

    void found(const PluginDescription& description) { answer(Object{{"plugin", writePlugin(description)}}); }

    void refuse(const std::string& why)
    {
        answer(Object{
            {"setAside", Object{{"plugin", writePlugin(plugins[next])},
                                {"failure", writeFailure({PluginFailure::Kind::refused, currentStep(), 0, why})}}}});
    }

    // Lets the plugin go, taken down on the main thread before the next is begun.
    void following()
    {
        plugin.reset();
        ++next;
        scanWorkerLoop::post([this] { nextPlugin(); });
    }
};

// A request under way, as the program follows it.
struct Underway
{
    std::int64_t number = 0;
    std::string format;
    std::filesystem::path file;
    std::string step = "starting the scan";
    std::int64_t count = 0;
    Steady::time_point progressed{};
    std::set<PluginIdentity> answered{};
};

PluginFailure failureFrom(const ProcessEnding& ending, const std::string& step)
{
    PluginFailure failure;
    failure.step = step;
    switch (ending.kind)
    {
        case ProcessEnding::Kind::signal:
        case ProcessEnding::Kind::exception:
            failure.kind = ending.killed ? PluginFailure::Kind::noAnswer : PluginFailure::Kind::crashed;
            failure.code = ending.code;
            failure.detail = ending.kind == ProcessEnding::Kind::signal
                                 ? "It ended on signal " + std::to_string(ending.code)
                                 : "It ended on exception " + std::to_string(ending.code);
            break;
        case ProcessEnding::Kind::exited:
            failure.kind = PluginFailure::Kind::noAnswer;
            failure.detail = "It ended with exit code " + std::to_string(ending.code) + " without answering.";
            break;
        case ProcessEnding::Kind::unknown:
            failure.kind = PluginFailure::Kind::noAnswer;
            failure.detail = "It ended without answering.";
            break;
    }
    return failure;
}

}

struct PluginScanWorker::Shared : std::enable_shared_from_this<PluginScanWorker::Shared>
{
    Options options;
    std::unique_ptr<ChildProcess> child;
    std::int64_t id = 0;

    // The program's object, while it wants to be told anything: the main thread's alone.
    PluginScanWorker* owner = nullptr;

    std::mutex lock;
    std::condition_variable wake;
    std::thread watcher;
    bool stopping = false;
    std::int64_t requests = 0;
    std::optional<Underway> underway;
    bool ended = false;
    bool lost = false;
    std::optional<Steady::time_point> endingSeen;
    std::optional<Steady::time_point> lostAt;

    // Tells the program's object on its main thread, if it is still there.
    void tell(std::function<void(PluginScanWorker&)> what)
    {
        options.post([weak = weak_from_this(), what = std::move(what)] {
            if (const auto self = weak.lock(); self != nullptr && self->owner != nullptr)
                what(*self->owner);
        });
    }

    // A message from the worker, on the link's thread: used only when it is whole, for the request under way.
    void received(const std::string& message)
    {
        const auto value = json::tryParse(message);
        if (!value || !value->isObject() || !(*value)["request"].isInteger())
            return;

        const std::lock_guard held(lock);
        if (!underway || underway->number != (*value)["request"].asInt())
            return;
        auto& now = *underway;
        const auto& object = value->asObject();
        try
        {
            if (const auto* step = object.find("step"))
            {
                auto text = step->asString();
                const auto count = required(object, "count").asInt();
                if (count == now.count)
                    return;
                now.count = count;
                now.progressed = Steady::now();
                if (text != now.step)
                {
                    now.step = text;
                    tell([text](PluginScanWorker& worker) {
                        if (worker.onProgress)
                            worker.onProgress(text);
                    });
                }
            }
            else if (const auto* plugin = object.find("plugin"))
            {
                auto description = readPlugin(*plugin, now.format, now.file);
                now.progressed = Steady::now();
                if (!now.answered.insert(description.identity).second)
                    return;
                tell([description](PluginScanWorker& worker) {
                    if (worker.onPlugin)
                        worker.onPlugin(description);
                });
            }
            else if (const auto* setAside = object.find("setAside"))
            {
                const auto& entry = setAside->asObject();
                PluginSetAside set{readPlugin(required(entry, "plugin"), now.format, now.file),
                                   readFailure(required(entry, "failure"))};
                now.progressed = Steady::now();
                if (!now.answered.insert(set.plugin.identity).second)
                    return;
                tell([set](PluginScanWorker& worker) {
                    if (worker.onSetAside)
                        worker.onSetAside(set);
                });
            }
            else if (const auto* done = object.find("done"))
            {
                const auto outcome = valueOf(*done, outcomes);
                std::optional<PluginFailure> why;
                if (const auto* failure = object.find("failure"))
                    why = readFailure(*failure);
                underway.reset();
                tell([outcome, why](PluginScanWorker& worker) {
                    if (worker.onDone)
                        worker.onDone(outcome, why);
                });
            }
        }
        catch (const std::exception&)
        {
            // A message that can't be used is passed over, as if never sent.
        }
    }

    // Follows the worker from a thread of its own: kills it once it has gone too long without progress, and says it
    // failed once it has ended before answering, after what it sent before it ended has all arrived.
    void watch()
    {
        std::unique_lock held(lock);
        while (!stopping)
        {
            wake.wait_for(held, std::chrono::milliseconds(20));
            if (stopping || ended)
                continue;

            if (underway && Steady::now() - underway->progressed > options.limit)
            {
                PluginFailure failure{PluginFailure::Kind::timedOut, underway->step, options.limit.count(),
                                      "It made no progress for " + std::to_string(options.limit.count()) + " ms."};
                underway.reset();
                ended = true;
                held.unlock();
                child->kill();
                held.lock();
                tell([failure](PluginScanWorker& worker) {
                    if (worker.onFailed)
                        worker.onFailed(failure);
                });
                continue;
            }

            held.unlock();
            const auto ending = child->ending();
            held.lock();
            if (ending)
            {
                // What it sent before it ended comes before its link is lost.
                if (!endingSeen)
                    endingSeen = Steady::now();
                if (!lost && Steady::now() - *endingSeen < std::chrono::seconds(1))
                    continue;
                ended = true;
                if (underway)
                {
                    const auto failure = failureFrom(*ending, underway->step);
                    underway.reset();
                    tell([failure](PluginScanWorker& worker) {
                        if (worker.onFailed)
                            worker.onFailed(failure);
                    });
                }
                continue;
            }

            // A worker whose link is lost ends by itself; one that doesn't is ended.
            if (lost && lostAt && Steady::now() - *lostAt > std::chrono::seconds(2))
            {
                held.unlock();
                child->kill();
                held.lock();
            }
        }
    }

    // Stops everything that could tell the program anything, leaving the process to the caller.
    void stop()
    {
        if (auto* link = child->link())
            link->close();
        {
            const std::lock_guard held(lock);
            stopping = true;
        }
        wake.notify_all();
        if (watcher.joinable())
            watcher.join();
    }
};

bool PluginScanWorker::isWorker(int argc, const char* const* argv)
{
    if (argc < 2 || argv[1] == nullptr || std::string_view(argv[1]) != workerArgument)
        return false;
    scanWorkerLoop::begin();
    return true;
}

void PluginScanWorker::post(std::function<void()> function)
{
    scanWorkerLoop::post(std::move(function));
}

bool PluginScanWorker::onMainThread()
{
    return scanWorkerLoop::onMainThread();
}

void PluginScanWorker::work(std::vector<PluginFormat*> formats)
{
    std::error_code error;
    auto link = ProcessLink::toParent(error);
    if (link == nullptr)
    {
        std::fprintf(stderr, "Scan worker: no link to the program: %s\n", error.message().c_str());
        system::endNow(70);
    }

    // Lives for as long as the process: the process ends without taking anything down.
    auto* worker = new Worker(std::move(formats), *link);
    link->onMessage([worker](std::string message) { worker->received(message); });
    link->onLost([] { system::endNow(0); });
    link->start();
    link.release();
    scanWorkerLoop::run();
}

std::unique_ptr<PluginScanWorker> PluginScanWorker::start(Options options, std::error_code& error)
{
    ChildProcess::Options process;
    process.arguments = {std::string(workerArgument)};
    process.environment = options.environment;
    process.outputFile = options.outputFile;
    process.link = true;

    auto shared = std::make_shared<Shared>();
    shared->child = ChildProcess::start(options.program, process, error);
    if (shared->child == nullptr)
        return nullptr;
    shared->options = std::move(options);
    shared->id = shared->child->id();

    auto* link = shared->child->link();
    link->onMessage([weak = std::weak_ptr(shared)](std::string message) {
        if (const auto self = weak.lock())
            self->received(message);
    });
    link->onLost([weak = std::weak_ptr(shared)] {
        if (const auto self = weak.lock())
        {
            const std::lock_guard held(self->lock);
            self->lost = true;
            self->lostAt = Steady::now();
            self->wake.notify_all();
        }
    });
    link->start();
    shared->watcher = std::thread([raw = shared.get()] { raw->watch(); });

    std::unique_ptr<PluginScanWorker> worker(new PluginScanWorker(shared));
    shared->owner = worker.get();
    return worker;
}

PluginScanWorker::PluginScanWorker(std::shared_ptr<Shared> from) : shared(std::move(from)) {}

PluginScanWorker::~PluginScanWorker()
{
    close();
}

bool PluginScanWorker::scan(const PluginScanRequest& request)
{
    std::int64_t number = 0;
    {
        const std::lock_guard held(shared->lock);
        if (shared->ended || shared->stopping || shared->underway)
            return false;
        number = ++shared->requests;
        shared->underway = Underway{number, request.format, request.file};
        shared->underway->progressed = Steady::now();
    }

    Array skip;
    for (const auto& identity : request.skip)
        skip.push_back(Object{{"format", identity.format}, {"id", identity.id}});
    const auto message = json::write(Object{{"request", number},
                                            {"format", request.format},
                                            {"file", paths::toUtf8(request.file)},
                                            {"rate", request.setup.rate},
                                            {"frames", request.setup.maximumFrames},
                                            {"skip", std::move(skip)}});
    if (!shared->child->link()->send(message))
    {
        const std::lock_guard held(shared->lock);
        shared->underway.reset();
        return false;
    }
    return true;
}

bool PluginScanWorker::isBusy() const
{
    const std::lock_guard held(shared->lock);
    return shared->underway.has_value();
}

bool PluginScanWorker::hasEnded() const
{
    const std::lock_guard held(shared->lock);
    return shared->ended;
}

std::int64_t PluginScanWorker::processId() const
{
    return shared->id;
}

ProcessEnding PluginScanWorker::close(std::chrono::milliseconds limit)
{
    shared->owner = nullptr;
    shared->stop();
    {
        const std::lock_guard held(shared->lock);
        shared->ended = true;
        shared->underway.reset();
    }
    auto ending = shared->child->wait(limit);
    if (!ending)
    {
        shared->child->kill();
        ending = shared->child->ending();
    }
    return ending.value_or(ProcessEnding{});
}

}
