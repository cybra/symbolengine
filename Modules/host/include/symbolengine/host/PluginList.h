#pragma once

#include "PluginFormat.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace symbolengine
{

/** Why a scan set a plugin aside, so it isn't loaded again until its file changes or the user asks. */
struct PluginFailure
{
    enum class Kind
    {
        /** It crashed its worker, with the signal's number on a Mac or Linux, or the exception's code on Windows. */
        crashed,

        /** It made no progress within the limit, in milliseconds. */
        timedOut,

        /** Its worker ended without answering, however it ended. */
        noAnswer,

        /** A step failed and the plugin, its code or the system said why, as when its code won't load or it can't be
            made or set up. */
        refused,
    };

    Kind kind = Kind::crashed;

    /** The step it came at, as its worker last said it, such as "loading the code" or "making plugin 3 of 40". */
    std::string step{};

    /** The signal, the exception's code or the limit, as its kind says; 0 where there is none. */
    std::int64_t code = 0;

    /** More in words, such as the first lines of a crash's backtrace. */
    std::string detail{};

    /** When it was set aside, and the program's version and architecture then, which the list fills in. */
    std::chrono::system_clock::time_point date{};
    std::string programVersion{};
    std::string architecture{};

    bool operator==(const PluginFailure&) const = default;
};

/** A plugin a scan set aside inside a file holding others, as one of a module's plugins that crashed when made. */
struct PluginSetAside
{
    PluginDescription plugin{};
    PluginFailure failure{};

    bool operator==(const PluginSetAside&) const = default;
};

/** One file a scan looked at, and what it found there. */
struct PluginFile
{
    enum class Outcome
    {
        /** It holds the plugins listed, with any set aside among them. */
        found,

        /** It holds no plugins, as a shell holds none whose maker's license isn't activated. */
        nothing,

        /** It is built only for architectures this program can't load. */
        otherArchitecture,

        /** Its code no longer matches its signature, so the system stops what loads it; reinstalling it mends it. It
            is looked at again when it changes, and after the computer restarts, since a Mac keeps what it found of a
            signature until then. */
        changedAfterSigning,

        /** The system refuses it as not notarized by its maker. */
        notNotarized,

        /** Loading it failed before it named any plugin, as failure says. */
        setAside,

        /** It is installed, but the system doesn't know it yet, as an Audio Unit its registry hasn't caught up with; it
            is looked at again at every scan. */
        notYetSeen,
    };

    /** Where it lies, the real path a scan reached it by, and its format's name. */
    std::filesystem::path path{};
    std::string format{};

    /** Whether it lies in the user's own folder, or one the user added, rather than the system's. */
    bool userFolder = false;

    PluginStamp stamp{};

    /** The architectures its binary is built for, as its headers say, such as "arm64" and "x86_64"; none where they
        weren't read. */
    std::vector<std::string> architectures{};

    Outcome outcome = Outcome::found;

    /** Why it was set aside, with Outcome::setAside, and which of its code was found changed, with
        Outcome::changedAfterSigning where a check found it. */
    std::optional<PluginFailure> failure{};

    /** The plugins in it the scan found, each located at this file, and those it set aside. */
    std::vector<PluginDescription> plugins{};
    std::vector<PluginSetAside> setAside{};

    /** Whether its scan finished; one cut short, as by the program quitting, is scanned again. */
    bool complete = true;

    /** False while the folder it lies in can't be read, as on a drive that isn't connected; it is kept as it was. */
    bool connected = true;

    /** Whether the user asked for it to be scanned again. */
    bool again = false;

    bool operator==(const PluginFile&) const = default;
};

/** One plugin in the list, known by its identity, with every place it was found. */
struct PluginEntry
{
    /** The place a program loads it from, chosen by a fixed rule: a place that can be reached, then one built for the
        program's own architecture, then the newer version, then the user's folder, then the path. */
    PluginDescription plugin{};

    /** The other places it was found, in the order of the same rule. */
    std::vector<PluginDescription> alternates{};

    /** Whether the place chosen can be reached: false while every place it was found is on a drive not connected. */
    bool connected = true;

    /** When the list first found it, anywhere, which its moving to another folder keeps; and when the file it is
        loaded from last changed. Scanning it again unchanged changes neither. */
    std::chrono::system_clock::time_point firstSeen{};
    std::chrono::system_clock::time_point updated{};

    bool operator==(const PluginEntry&) const = default;
};

/** The plugins a program's scans found, kept in a file between runs, so a program knows them without loading any.
    Plugins set aside are kept with why, and scanned again only when their file changes or the user asks.

    The file is JSON with a format version and one entry to a line, written safely with the last good one kept beside
    it, its name ending in ".backup". It is read entry by entry, so damage costs only the entries it touches, and the
    entries a damaged file has lost are taken from the one kept. Fields this version doesn't know are kept in every
    entry it doesn't change, for a later version, which only ever adds to them. A program keeps a list for each
    architecture it runs as (system::architecture()), since a universal program run through Rosetta loads other
    plugins. Used on one thread at a time. */
class PluginList
{
public:
    struct Options
    {
        std::filesystem::path file;

        /** The program's version, kept with each plugin set aside. */
        std::string programVersion;

        /** Told of each entry left out as unreadable, and of entries taken from the file kept. */
        std::function<void(const std::string&)> log;

        /** The time, and when the computer started, for tests; the system's own without them. */
        std::function<std::chrono::system_clock::time_point()> now;
        std::function<std::chrono::system_clock::time_point()> computerStarted;
    };

    /** Reads the list's file; a missing or unreadable one starts the list empty. */
    explicit PluginList(Options options);

    /** Lets the list go without saving it: only save() writes. */
    ~PluginList();

    PluginList(const PluginList&) = delete;
    PluginList& operator=(const PluginList&) = delete;

    /** Every plugin found, in order of identity. */
    std::vector<PluginEntry> plugins() const;

    /** The plugin with this identity, wherever it lies now. */
    std::optional<PluginEntry> find(const PluginIdentity& identity) const;

    /** The plugins of a format with this maker and name, for a program to ask about loading when a plugin it wants
        isn't found by identity: a guess, which it never loads without asking. */
    std::vector<PluginEntry> findByName(std::string_view format, std::string_view maker, std::string_view name) const;

    /** Every file scanned, in order of path, and the one at a path. */
    std::vector<PluginFile> files() const;
    std::optional<PluginFile> file(const std::filesystem::path& path) const;

    /** Whether a file a scan came to is to be scanned: one new to the list, changed since by its stamp, cut short,
        asked for again, or changed after signing with the computer started again since. A file with the same stamp
        isn't, whatever it held, so a plugin set aside stays set aside. */
    bool needsScan(const std::filesystem::path& path, const PluginStamp& stamp) const;

    /** Records what a scan found in a file, in place of what the list held of it, filling in each failure's date, the
        program's version and its architecture. */
    void record(PluginFile file);

    /** Says a folder of plugins was read through, its subfolders included, and what files were found there: files the
        list holds in it that weren't found leave it, and those found are connected again. A folder no longer scanned
        is read with nothing found. */
    void folderRead(const std::filesystem::path& folder, const std::vector<std::filesystem::path>& found);

    /** Says a folder of plugins can't be read, as on a drive that isn't connected: its files are kept as they were,
        marked as not connected. */
    void folderUnreadable(const std::filesystem::path& folder);

    /** Asks for a file to be scanned again at the next scan, as Try Again does for one set aside. */
    void tryAgain(const std::filesystem::path& path);

    /** Forgets every file, so the next scan scans everything again, keeping when each plugin was first found. */
    void clear();

    /** Whether a scan was under way when the list was last saved, so one the program quit during, or was killed in,
        carries on at its next start. A scan says so as it begins, and that it isn't once it ends or is stopped. */
    bool scanUnderway() const;
    void setScanUnderway(bool underway);

    /** Whether anything has changed since the file was read or last saved, or the file read was damaged. */
    bool hasChanges() const;

    /** Writes the list to its file if anything has changed, the file before becoming the backup unless it was
        damaged. Throws std::filesystem::filesystem_error, or, given a std::error_code, sets it instead. */
    void save();
    void save(std::error_code& error);

    struct Inner;

private:
    std::unique_ptr<Inner> inner;
};

}
