#pragma once

#include "PluginFormat.h"

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace symbolengine
{

class Plugin;

/** What happened to a hosted plugin, told on the main thread. */
enum class PluginChange
{
    /** It was made and its layout settled, ready to be prepared, or it failed to be made, as problem() says. */
    made,

    /** Its latency or tail changed, and it was prepared again to apply it. */
    latency,

    /** Its buses changed, and its layout was settled again. */
    buses,

    /** Its parameters changed: which there are, their names, ranges or flags. */
    parameters,

    /** Its parameters' values changed together, as by a preset chosen in its own window. */
    values,

    /** Its state changed outside its parameters, so the document holding it should be saved. */
    state,

    /** It asks to be made again, which is for the program to do. */
    reload,

    /** A call into it failed or threw: it is no longer called, the audio passes it by, and problem() says why. */
    failed,

    /** It didn't leave a process call within the limit, so it is left out of the audio and not called again. */
    stuck,

    /** A call into it changed what belongs to the whole process, the floating-point mode, the C locale or the working
        folder, which was put back. */
    processState,
};

/** What every plugin a program hosts shares: the main thread they are made and called on, and the queue that makes
    them one per turn of its run loop, so a slow one never holds up the others or the program's windows. */
class PluginHost
{
public:
    struct Options
    {
        /** Runs a function on the main thread, after those posted before it: in a program with a message loop,
            ui::messageLoop::callAsync. */
        std::function<void(std::function<void()>)> post;

        /** Whether the calling thread is the main thread: ui::messageLoop::isMessageThread in a program with one. */
        std::function<bool()> onMainThread;

        /** How long the main thread waits for the audio thread to leave a plugin it is taking out of processing. */
        std::chrono::milliseconds audioLimit{2000};

        /** Told what was wrong when the program calls a plugin on the wrong thread or in the wrong state, which is
            refused. Without it a debug build stops there, so the mistake is seen at once. */
        std::function<void(const std::string&)> misuse;
    };

    explicit PluginHost(Options options);
    ~PluginHost();

    PluginHost(const PluginHost&) = delete;
    PluginHost& operator=(const PluginHost&) = delete;

    /** Makes a plugin of the format later, on the main thread, after those asked for before it; it says
        PluginChange::made once it is made or has failed. The last hold on it can be let go of on any thread, and it
        is taken down on the main thread. */
    std::shared_ptr<Plugin> make(PluginFormat& format, const PluginDescription& plugin);

    struct Shared;

private:
    std::shared_ptr<Shared> shared;
};

/** One plugin a program hosts, made by PluginHost::make. Its calls are the main thread's, except process(), which is
    the audio thread's; a call on the wrong thread or in the wrong state is refused and told to Options::misuse. A call
    made while another into the plugin is under way, as one a plugin's own dialog lets run, is refused too: whenFree()
    makes it once it can be made. */
class Plugin
{
public:
    enum class State
    {
        making,

        /** Made, its layout settled, not set up. */
        inactive,

        /** Set up and activated: it processes the blocks process() is given. */
        active,

        /** It failed to be made, or a call into it failed: it isn't called again, and problem() says why. */
        failed,
    };

    ~Plugin();

    Plugin(const Plugin&) = delete;
    Plugin& operator=(const Plugin&) = delete;

    State state() const;
    const PluginDescription& description() const;
    PluginError problem() const;

    /** Told on the main thread of what happens to it. Set as make() returns, before it is made. */
    std::function<void(PluginChange)> onChange;

    /** The layout it settled on: stereo where it takes it, else mono in and stereo out, else mono, else its own. */
    PluginLayout layout() const;

    /** Sets it up and activates it, taking it out of processing first if it is active; false, with problem() set, if
        it can't be. */
    bool prepare(const PluginSetup& setup);

    /** Deactivates it. */
    void release();

    /** Clears what it holds of audio already played, keeping its settings. */
    bool reset();

    /** Its latency and tail in frames, as read when it was last prepared. */
    int latency() const;
    int tail() const;

    /** Its state for the use, or nothing with problem() set; and giving it back. */
    std::optional<std::string> saveState(PluginStateUse use);
    bool loadState(std::string_view state, PluginStateUse use);

    /** Whether a call into it is under way on the main thread, inside which a plugin can turn the run loop, as a
        dialog does. */
    bool isBusy() const;

    /** Runs the function on the main thread once no call into the plugin is under way: at once if none is. */
    void whenFree(std::function<void()> then);

    /** Plays a block through it in place on the audio thread, as many channels as the program has, the plugin's
        layout fitted to them. While it can't be called, being made, prepared or taken down, or failed, the block
        passes as it came. */
    void process(float* const* channels, int channelCount, int frames) noexcept;

    struct Inner;

private:
    friend class PluginHost;
    explicit Plugin(std::shared_ptr<Inner> inner);

    std::shared_ptr<Inner> inner;
};

}
