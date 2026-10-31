#include "symbolengine/core/SettingsFile.h"

#include "symbolengine/core/Files.h"
#include "symbolengine/core/Json.h"

namespace symbolengine
{

SettingsFile::SettingsFile(std::filesystem::path file) : path(std::move(file))
{
    std::error_code error;
    const auto text = files::read(path, error);

    if (error == std::errc::no_such_file_or_directory)
        return;

    if (error)
    {
        unread = error;
        return;
    }

    if (auto parsed = json::tryParse(text); parsed && parsed->isObject())
    {
        settings = std::move(parsed->asObject());
        return;
    }

    auto aside = path;
    aside += ".unreadable";
    std::filesystem::rename(path, aside, unread);
}

SettingsFile::~SettingsFile()
{
    try
    {
        std::error_code ignored;
        save(ignored);
    }
    catch (...)
    {
    }
}

Value SettingsFile::get(std::string_view name) const
{
    std::scoped_lock hold(lock);
    const auto* found = settings.find(name);
    return found != nullptr ? *found : Value();
}

void SettingsFile::set(std::string_view name, Value value)
{
    std::scoped_lock hold(lock);
    if (const auto* existing = settings.find(name); existing != nullptr && *existing == value)
        return;
    settings.set(name, std::move(value));
    changed = true;
}

bool SettingsFile::remove(std::string_view name)
{
    std::scoped_lock hold(lock);
    if (!settings.erase(name))
        return false;
    changed = true;
    return true;
}

Object SettingsFile::all() const
{
    std::scoped_lock hold(lock);
    return settings;
}

std::error_code SettingsFile::readProblem() const
{
    return unread;
}

bool SettingsFile::hasChanges() const
{
    std::scoped_lock hold(lock);
    return changed;
}

void SettingsFile::save(std::error_code& error)
{
    std::scoped_lock hold(lock);
    saveLocked(error);
}

void SettingsFile::save()
{
    std::error_code error;
    save(error);
    if (error)
        throw std::filesystem::filesystem_error("can't save the settings", path, error);
}

void SettingsFile::saveLocked(std::error_code& error)
{
    error.clear();
    if (!changed)
        return;

    if (unread)
    {
        error = unread;
        return;
    }

    files::writeSafely(path, json::write(settings, json::Layout::pretty) + "\n", error);
    if (!error)
        changed = false;
}

}
