#pragma once

#include "PluginFormat.h"

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace symbolengine
{

class PluginCrashNote;

namespace crashNote
{
class InCall;
int claim(PluginCrashNote* note, const PluginDescription& plugin);
void release(PluginCrashNote* note, int slot);
}

/** How a program's last run ended, as its crash note kept it: cleanly, or in the middle of calls into plugins, each
    with the plugin, the call and the thread, and, where the crash handler caught the crash, what it was. */
struct PluginRunEnding
{
    /** A call into a plugin under way as the run ended. */
    struct Inside
    {
        PluginDescription plugin;
        std::string call;
        bool audioThread = false;

        /** Whether the crash came on the thread making this call. */
        bool crashed = false;
    };

    /** Whether it ended as programs end, or the note was never begun or can't be read. */
    bool clean = true;

    std::vector<Inside> inside;

    /** What the program said it had open, such as a project's path. */
    std::string context;

    /** The crash's signal or exception and a backtrace, where the crash handler caught it. */
    std::string crash;

    /** The call the run most likely ended in: the one the crash came on, else one on the main thread, else one on an
        audio thread; none for a clean end or one outside every plugin's call. */
    const Inside* culprit() const;
};

/** A program's note of what it is doing with plugins, kept in a small file of its own, so its next run knows whether
    the last one ended inside a plugin, and which. The host records, before it makes a plugin and around every call into
    one, on the main thread and the audio thread, which plugin and which call, in memory the system writes to the file
    itself, so a run that ends in the middle of one, however it ends, the system stopping it outright among the ways,
    leaves the file saying so; each record is a few stores, cheap enough to stand around every block of audio. Opened
    as the program starts, before any plugin is made, and given to its PluginHost (PluginHost::setCrashNote), which it
    outlives. One per program. */
class PluginCrashNote
{
public:
    /** Opens the note in the file, reading what it says of the last run first, then begins it for this one. With
        catchCrashes, a handler for the process's crashes adds the crash, a backtrace and the thread it came on to the
        note, then lets the crash go on as it would have, to whatever handled it before and to the system's own report;
        on Windows, where a crash inside the host's own calls is caught already, it catches those on plugins' own
        threads. Nothing, with the error set, where the file can't be made. */
    static std::unique_ptr<PluginCrashNote> open(const std::filesystem::path& file, bool catchCrashes,
                                                 PluginError& error);

    /** Ends the note as a clean end, the crash handler let go of. */
    ~PluginCrashNote();

    PluginCrashNote(const PluginCrashNote&) = delete;
    PluginCrashNote& operator=(const PluginCrashNote&) = delete;

    /** How the last run ended. */
    const PluginRunEnding& lastRun() const;

    /** What the program has open, kept for the next run, such as a project's path: up to 1,000 bytes of it. On the main
        thread. */
    void setContext(std::string_view context);

    struct Impl;

private:
    friend class crashNote::InCall;
    friend int crashNote::claim(PluginCrashNote* note, const PluginDescription& plugin);
    friend void crashNote::release(PluginCrashNote* note, int slot);

    explicit PluginCrashNote(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl;
};

}
