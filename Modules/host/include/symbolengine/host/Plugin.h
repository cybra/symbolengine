#pragma once

#include "PluginFormat.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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

    /** It asks for its editor to be opened, or brought in front of the others if it is open, which is for the program
        to do. */
    editor,

    /** A call into it failed or threw: it is no longer called, the audio passes it by, and problem() says why. */
    failed,

    /** It didn't leave a process call within the limit, so it is left out of the audio and not called again. */
    stuck,

    /** It crashed in a call, on Windows, where the host caught the fault: it is switched off, the audio passing it by,
        and never called again, and problem() says in which call. The program's process can no longer be trusted, so
        it saves its document to a new file and asks to be restarted. Elsewhere a crash ends the program. */
    crashed,

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

    /** Whether a call into it crashed, which the program survives only on Windows, where the host catches the fault:
        the program's process can no longer be trusted. From any thread. */
    bool hasCrashed() const;

    /** Told on the main thread of what happens to it. Set as make() returns, before it is made. */
    std::function<void(PluginChange)> onChange;

    /** Told on the main thread of a parameter the plugin moved itself, and whether it was the user's doing, for which
        the program marks its document changed: a reported value isn't, nor is an edit within 250 ms of its state
        being given, in which time it saying its state changed isn't told either. */
    std::function<void(const PluginEdit& edit, bool byUser)> onEdit;

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

    /** Its parameters, as read when it was made and again when it said they changed. */
    std::vector<PluginParameter> parameters() const;

    /** A parameter's value, as the program last set it or the plugin last said it is. */
    double parameter(std::uint32_t id) const;

    /** Sets a parameter's value: the plugin's processing hears it at its next block, or, while nothing plays it,
        before its state is next taken or once it is next prepared; the plugin shows it at once. False for a parameter
        it doesn't have or says can't be written. */
    bool setParameter(std::uint32_t id, double value);

    /** A value of a parameter as the plugin says it in words. */
    std::string parameterText(std::uint32_t id, double value);

    /** Its state for the use, or nothing with problem() set, once its processing has heard every value set; and
        giving it back. */
    std::optional<std::string> saveState(PluginStateUse use);
    bool loadState(std::string_view state, PluginStateUse use);

    /** Whether a call into it is under way on the main thread, inside which a plugin can turn the run loop, as a
        dialog does. */
    bool isBusy() const;

    /** Runs the function on the main thread once no call into the plugin is under way: at once if none is. */
    void whenFree(std::function<void()> then);

    /** Whether it has an editor of its own that can be opened on this system, asked of it the first time, without
        opening one, and kept: false on Linux, where editors aren't hosted yet. */
    bool hasEditor();

    /** Plays a block through it in place on the audio thread, as many channels as the program has, the plugin's
        layout fitted to them. While it is held off the audio for a call that can't overlap its processing, as while
        it is prepared, given a state, reset or taken down, the block is silent, never heard unprocessed; before it is
        first prepared, or once it has failed, the block passes as it came, so the program plays past it. */
    void process(float* const* channels, int channelCount, int frames) noexcept;

    /** The same, with the program's own changes to its parameters for the block, as its automation gives them, in
        order of their frames, which the plugin hears at those frames and is shown on the main thread; and where the
        program is in time for the block's first frame. Without a time, the plugin is told the program isn't playing,
        at a position that counts the frames it has played. */
    void process(float* const* channels, int channelCount, int frames, const PluginParameterChange* changes,
                 int changeCount, const PluginTime* time = nullptr) noexcept;

    struct Inner;

private:
    friend class PluginHost;
    friend class PluginEditor;
    explicit Plugin(std::shared_ptr<Inner> inner);

    std::shared_ptr<Inner> inner;
};

/** A plugin's editor, open in a native view of the program's, which keeps its plugin while it is open. Made and used
    on the main thread. Destroying it closes the editor in its format's order while the view it is in is still whole,
    and lets go of the plugin a turn of the run loop later, so the plugin finishes what the editor started first.
    Destroyed while a call into the plugin is under way, as one a plugin's own dialog lets run, it closes the editor
    once the call returns, so the view it is in has to stay whole until then: Plugin::whenFree says when. */
class PluginEditor
{
public:
    /** Opens the plugin's editor in the native view, an NSView on a Mac or an HWND on Windows, already in its window,
        which the frame sizes to the editor before the editor is put in it, and again whenever the plugin asks. Nothing,
        with the error set, if the plugin has no editor, one of its editors is open, a call into it is under way or its
        editor can't be opened. */
    static std::unique_ptr<PluginEditor> open(std::shared_ptr<Plugin> plugin, void* view, PluginEditorFrame& frame,
                                              PluginError& error);

    ~PluginEditor();

    PluginEditor(const PluginEditor&) = delete;
    PluginEditor& operator=(const PluginEditor&) = delete;

    /** Its size, as it last said it or was told. */
    PluginEditorSize size() const;

    /** Whether the user may resize it, as it said when it opened. */
    bool resizable() const;

    /** The size nearest the one given that it takes, for a window the user is dragging to that size: the size it has
        for one that can't be resized. */
    PluginEditorSize constrain(PluginEditorSize wanted);

    /** Tells it the view it is in has been resized to the size, as by the user's drag. */
    void resized(PluginEditorSize size);

    /** Tells it the view it is in is on a screen of another scale, once the user's move there is over, so an editor
        sized in pixels, as on Windows, can take its size for that scale. */
    void rescaled(double scale);

    const std::shared_ptr<Plugin>& plugin() const { return held; }

private:
    PluginEditor(std::shared_ptr<Plugin> plugin, std::unique_ptr<PluginFormatEditor> editor);

    std::shared_ptr<Plugin> held;
    std::unique_ptr<PluginFormatEditor> editor;
    bool canResize = false;
};

}
