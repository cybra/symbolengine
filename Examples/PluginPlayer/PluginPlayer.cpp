// A small program that hosts another maker's plugin: it finds the effects in a VST3 file, makes the first, plays a few
// seconds of noise through it on an audio thread, and says how long that took and whether the plugin called anything a
// real-time thread never should. The program gives the host its main thread, here a queue of its own; a program with a
// message loop gives it ui::messageLoop::callAsync.

#include <symbolengine/core.h>
#include <symbolengine/host.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

using namespace symbolengine;
using namespace std::chrono_literals;

namespace
{

class MainThread
{
public:
    void post(std::function<void()> task)
    {
        const std::lock_guard held(lock);
        tasks.push_back(std::move(task));
        arrived.notify_all();
    }

    bool isCurrent() const { return std::this_thread::get_id() == owner; }

    // Runs what was posted until the condition holds, or the time is up.
    template <typename Condition>
    bool runUntil(Condition condition, std::chrono::seconds limit)
    {
        const auto until = std::chrono::steady_clock::now() + limit;

        while (!condition())
        {
            std::unique_lock held(lock);

            if (!arrived.wait_until(held, until, [this] { return !tasks.empty(); }))
                return false;

            auto next = std::move(tasks.front());
            tasks.pop_front();
            held.unlock();
            next();
        }

        return true;
    }

private:
    const std::thread::id owner = std::this_thread::get_id();
    std::mutex lock;
    std::condition_variable arrived;
    std::deque<std::function<void()>> tasks;
};

}

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::printf("Give it a VST3 plugin file.\n");
        return 2;
    }

    MainThread main;
    const auto post = [&main](std::function<void()> task) { main.post(std::move(task)); };
    const auto onMainThread = [&main] { return main.isCurrent(); };

    Vst3Format vst3{{"SymbolEngine Plugin Player", post, onMainThread}};

    PluginHost::Options options{post, onMainThread, 2s, nullptr};
    options.watchUnsafeCalls = true;
    PluginHost host{options};

    // What the file holds, each plugin as it describes itself. A program scans in worker processes instead, so a plugin
    // that crashes as it loads takes down only a worker.
    PluginError error;
    const auto found = vst3.pluginsIn(std::filesystem::path(argv[1]), error);
    const auto effect = std::find_if(found.begin(), found.end(),
                                     [](const PluginDescription& one) { return one.kind == PluginKind::effect; });

    if (effect == found.end())
    {
        std::printf("No effect in it. %s\n", error.message.c_str());
        return 1;
    }

    // Made on the main thread, later: the queue has to turn for it.
    const auto plugin = host.make(vst3, *effect);

    if (!main.runUntil([&] { return plugin->state() != Plugin::State::making; }, 60s) ||
        plugin->state() == Plugin::State::failed)
    {
        std::printf("%s didn't load. %s\n", effect->name.c_str(), plugin->problem().message.c_str());
        return 1;
    }

    constexpr auto frames = 512;

    if (!plugin->prepare({48000.0, frames, false}))
    {
        std::printf("%s can't be prepared. %s\n", effect->name.c_str(), plugin->problem().message.c_str());
        return 1;
    }

    std::printf("%s by %s: %d in, %d out, latency %d frames\n", effect->name.c_str(), effect->maker.c_str(),
                plugin->layout().inputs, plugin->layout().outputs, plugin->latency());

    // Only process() is the audio thread's. The main thread goes on turning, as what a plugin tells the program comes
    // to it there.
    bool finished = false;
    std::thread audio([&] {
        std::mt19937 random(1);
        std::uniform_real_distribution<float> noise(-0.25f, 0.25f);
        std::vector<float> left(frames), right(frames);
        float* channels[] = {left.data(), right.data()};

        for (auto block = 0; block < 500; ++block)
        {
            for (auto i = 0; i < frames; ++i)
            {
                left[static_cast<std::size_t>(i)] = noise(random);
                right[static_cast<std::size_t>(i)] = noise(random);
            }

            plugin->process(channels, 2, frames);
        }

        post([&finished] { finished = true; });
    });
    main.runUntil([&finished] { return finished; }, 120s);
    audio.join();

    const auto timing = plugin->takeTiming();
    std::printf("%lld blocks, taking %.1f%% of their time, the worst %.1f%% of one\n",
                static_cast<long long>(timing.blocks), timing.load * 100.0, timing.worstShare * 100.0);

    if (!PluginHost::canWatchUnsafeCalls())
        std::printf("This program can't watch for unsafe calls.\n");

    for (const auto& call : plugin->unsafeCalls())
    {
        if (!call.inPluginCode)
            continue;

        std::printf("In %s it called %s, %lld times in %lld blocks, from block %lld to %lld:\n", call.step.c_str(),
                    call.function.c_str(), static_cast<long long>(call.times), static_cast<long long>(call.blocks),
                    static_cast<long long>(call.firstBlock), static_cast<long long>(call.lastBlock));

        for (const auto& frame : call.backtrace)
            std::printf("    %s\n", frame.c_str());
    }

    plugin->release();
    return plugin->state() == Plugin::State::failed ? 1 : 0;
}
