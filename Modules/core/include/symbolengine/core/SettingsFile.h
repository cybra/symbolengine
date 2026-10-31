#pragma once

#include "Value.h"

#include <filesystem>
#include <mutex>
#include <string_view>
#include <system_error>

namespace symbolengine
{

/** Named settings kept in a JSON file, such as a program's preferences, saved with files::writeSafely so a crash
    never leaves them half written. Any thread except the audio thread may use it.

    A missing file starts empty. A file that reads as something other than settings is moved aside, its name ending in
    ".unreadable", and the settings start empty, so the next save doesn't destroy what the user had. A file the system
    wouldn't read, or one that couldn't be moved aside, may hold good settings: it is left as it is, the settings start
    empty, and saving refuses (readProblem()), so a passing failure costs this run's changes and never the file. */
class SettingsFile
{
public:
    explicit SettingsFile(std::filesystem::path file);

    /** Saves any changes, ignoring errors; call save() first to know whether it worked. */
    ~SettingsFile();

    SettingsFile(const SettingsFile&) = delete;
    SettingsFile& operator=(const SettingsFile&) = delete;

    const std::filesystem::path& file() const noexcept { return path; }

    /** The setting with this name, or null if there is none. */
    Value get(std::string_view name) const;

    /** Sets a setting. Setting the value it already has changes nothing. */
    void set(std::string_view name, Value value);

    /** Removes a setting, and says whether there was one. */
    bool remove(std::string_view name);

    /** All the settings. */
    Object all() const;

    /** Why the file there couldn't be read or moved aside, so nothing is saved over it; nothing for any other. */
    std::error_code readProblem() const;

    /** Whether anything has changed since the file was read or last saved. */
    bool hasChanges() const;

    /** Writes the settings to the file if anything has changed. */
    void save();
    void save(std::error_code& error);

private:
    std::filesystem::path path;
    mutable std::mutex lock;
    Object settings;
    bool changed = false;
    std::error_code unread;

    void saveLocked(std::error_code& error);
};

}
