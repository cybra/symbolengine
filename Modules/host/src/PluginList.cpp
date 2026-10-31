#include "symbolengine/host/PluginList.h"

#include "PluginJson.h"

#include <symbolengine/core/Files.h>
#include <symbolengine/core/Json.h>
#include <symbolengine/core/Paths.h>
#include <symbolengine/core/System.h>
#include <symbolengine/core/Text.h>
#include <symbolengine/core/Value.h>

#include <algorithm>
#include <map>
#include <set>
#include <utility>

namespace symbolengine
{

namespace
{

using namespace pluginJson;

// The list file's format. A later version that only adds fields keeps the number; one that changes what a field means
// moves entries over from the formats before it as it reads them.
constexpr std::int64_t currentFormat = 1;

// How far apart two readings of when the computer started may be and still be the same start, in milliseconds:
// Windows' reading moves with adjustments to the clock.
constexpr std::uint64_t sameStart = 10'000;

// Whether two readings of when the computer started are of different starts, whatever a damaged file says of one.
bool differentStarts(std::int64_t a, std::int64_t b)
{
    const auto apart = a > b ? static_cast<std::uint64_t>(a) - static_cast<std::uint64_t>(b)
                             : static_cast<std::uint64_t>(b) - static_cast<std::uint64_t>(a);
    return apart > sameStart;
}

// The key a file is kept by, so the same path written with either separator on Windows is the same file.
std::string keyOf(const std::filesystem::path& path)
{
    return paths::toUtf8(path.lexically_normal());
}

// Whether a path lies inside a folder, at any depth: by whole names, so "VST3 Extra" isn't inside "VST3".
bool inside(const std::filesystem::path& path, std::filesystem::path folder)
{
    // A folder with no name, as a failed look at one gives, holds nothing, where every path starts with no names.
    if (folder.empty())
        return false;

    folder = folder.lexically_normal();
    if (!folder.has_filename())
        folder = folder.parent_path();
    const auto normal = path.lexically_normal();
    auto part = normal.begin();
    for (const auto& name : folder)
    {
        if (part == normal.end() || *part != name)
            return false;
        ++part;
    }
    return part != normal.end();
}

Object writeStamp(const PluginStamp& stamp)
{
    return Object{{"size", stamp.size},
                  {"modified", stamp.modified},
                  {"version", stamp.version},
                  {"manifestModified", stamp.manifestModified}};
}

PluginStamp readStamp(const Value& value)
{
    const auto& object = value.asObject();
    return {required(object, "size").asInt(), required(object, "modified").asInt(), textOr(object, "version"),
            numberOr(object, "manifestModified")};
}

// A file the list holds: what a scan found in it, when the computer had started when it was recorded, and the entry
// as it was read while it is unchanged, so fields this version doesn't know are written back.
struct Record
{
    PluginFile file;
    std::int64_t computerStarted = 0;
    std::optional<Object> read;
};

Object writeFile(const Record& record)
{
    const auto& file = record.file;
    Object object{{"path", paths::toUtf8(file.path)},
                  {"format", file.format},
                  {"userFolder", file.userFolder},
                  {"stamp", writeStamp(file.stamp)}};
    Array architectures;
    for (const auto& architecture : file.architectures)
        architectures.push_back(architecture);
    object.set("architectures", std::move(architectures));
    object.set("outcome", nameOf(file.outcome, outcomes));
    if (file.failure)
        object.set("failure", writeFailure(*file.failure));
    Array plugins;
    for (const auto& plugin : file.plugins)
        plugins.push_back(writePlugin(plugin));
    object.set("plugins", std::move(plugins));
    Array setAside;
    for (const auto& plugin : file.setAside)
        setAside.push_back(Object{{"plugin", writePlugin(plugin.plugin)}, {"failure", writeFailure(plugin.failure)}});
    object.set("setAside", std::move(setAside));
    object.set("complete", file.complete);
    object.set("connected", file.connected);
    if (!file.through.empty())
        object.set("through", paths::toUtf8(file.through));
    object.set("again", file.again);
    object.set("computerStarted", record.computerStarted);
    return object;
}

Record readFile(const Object& object)
{
    Record record;
    auto& file = record.file;
    file.path = paths::fromUtf8(required(object, "path").asString());
    file.format = required(object, "format").asString();
    if (file.path.empty() || file.format.empty())
        throw TypeError("it has no path or no format");
    file.userFolder = flagOr(object, "userFolder", false);
    file.stamp = readStamp(required(object, "stamp"));
    if (const auto* architectures = object.find("architectures"))
        for (const auto& architecture : architectures->asArray())
            file.architectures.push_back(architecture.asString());
    file.outcome = valueOf(required(object, "outcome"), outcomes);
    if (const auto* failure = object.find("failure"))
        file.failure = readFailure(*failure);
    if (const auto* plugins = object.find("plugins"))
        for (const auto& plugin : plugins->asArray())
            file.plugins.push_back(readPlugin(plugin, file.format, file.path));
    if (const auto* setAside = object.find("setAside"))
        for (const auto& plugin : setAside->asArray())
            file.setAside.push_back({readPlugin(required(plugin.asObject(), "plugin"), file.format, file.path),
                                     readFailure(required(plugin.asObject(), "failure"))});
    file.complete = flagOr(object, "complete", true);
    file.connected = flagOr(object, "connected", true);
    if (const auto* through = object.find("through"))
        file.through = paths::fromUtf8(through->asString());
    file.again = flagOr(object, "again", false);
    record.computerStarted = numberOr(object, "computerStarted");
    record.read = object;
    return record;
}

// When the list first found a plugin, kept by its identity whatever becomes of its files.
struct Seen
{
    std::int64_t firstSeen = 0;
    std::optional<Object> read;
};

// What one file of the list held: whole, when it was read as one document of the list's shape with every entry
// readable, so it may become the backup.
struct Contents
{
    bool whole = false;
    bool underway = false;
    std::map<std::string, Record> records;
    std::map<PluginIdentity, Seen> seen;
    Object otherMembers;
};

bool hasListShape(const Value& document)
{
    return document["format"].isInteger() && document["files"].isArray() && document["seen"].isArray();
}

}

struct PluginList::Inner
{
    Options options;
    std::filesystem::path backup;
    std::map<std::string, Record> records;
    std::map<PluginIdentity, Seen> seen;

    // The file's own members this version doesn't know, written back as they were read.
    Object otherMembers;

    bool underway = false;

    bool changed = false;

    // Whether the list's file was read whole or has been written since, so it may become the backup.
    bool fileIsGood = false;

    explicit Inner(Options from) : options(std::move(from))
    {
        backup = options.file;
        backup += ".backup";
    }

    Clock::time_point now() const { return options.now ? options.now() : Clock::now(); }

    std::int64_t computerStarted() const
    {
        return millisecondsOf(options.computerStarted ? options.computerStarted() : system::computerStarted());
    }

    void log(const std::string& line) const
    {
        if (options.log)
            options.log(line);
    }

    // Reads one entry into the contents, saying why if it can't, and gives whether it could.
    bool readEntry(const Value& entry, Contents& contents) const
    {
        std::string which = "an entry";
        try
        {
            const auto& object = entry.asObject();
            if (const auto* path = object.find("path"))
            {
                if (const auto* text = path->ifString())
                    which = "the entry for " + *text;
                auto record = readFile(object);
                const auto key = keyOf(record.file.path);
                if (!contents.records.emplace(key, std::move(record)).second)
                    throw TypeError("an entry before it has the same path");
                return true;
            }
            if (object.contains("firstSeen"))
            {
                const PluginIdentity identity{required(object, "format").asString(), required(object, "id").asString()};
                which = "the entry for " + identity.format + " " + identity.id;
                contents.seen.insert_or_assign(identity, Seen{required(object, "firstSeen").asInt(), object});
                return true;
            }
            throw TypeError("it is neither a file nor a plugin");
        }
        catch (const std::exception& error)
        {
            log("Left out " + which + " of the plugin list " + paths::toUtf8(options.file) +
                ", which can't be read: " + error.what());
            return false;
        }
    }

    Contents parse(std::string_view text, const std::filesystem::path& from) const
    {
        Contents contents;
        if (const auto document = json::tryParse(text); document && hasListShape(*document))
        {
            const auto& object = document->asObject();
            contents.whole = true;

            // A later version's: its entries may mean what this version can't tell. So it reads as nothing, and being whole
            // it is kept as the backup when this version saves.
            if ((*document)["format"].intOr(currentFormat) > currentFormat)
            {
                log("The plugin list " + paths::toUtf8(from) +
                    " is from a later version of the program, so it is scanned again.");
                return contents;
            }

            for (const auto& entry : object.find("files")->asArray())
                contents.whole = readEntry(entry, contents) && contents.whole;
            for (const auto& entry : object.find("seen")->asArray())
                contents.whole = readEntry(entry, contents) && contents.whole;
            contents.underway = object.find("scanUnderway") != nullptr && object.find("scanUnderway")->boolOr(false);
            for (const auto& member : object)
                if (member.name() != "format" && member.name() != "scanUnderway" && member.name() != "files" &&
                    member.name() != "seen")
                    contents.otherMembers.set(member.name(), member.value);
            return contents;
        }

        // Damaged: each line holding a whole entry is read by itself.
        log("The plugin list " + paths::toUtf8(from) + " is damaged; reading each entry that can still be read.");
        for (const auto line : text::split(text, "\n"))
        {
            auto entry = text::trim(line);
            if (entry.ends_with(','))
                entry.remove_suffix(1);
            if (entry == "\"scanUnderway\": true")
                contents.underway = true;
            if (entry.size() < 2 || entry.front() != '{' || entry.back() != '}')
                continue;
            if (const auto value = json::tryParse(entry); value && value->isObject())
                readEntry(*value, contents);
        }
        return contents;
    }

    void load()
    {
        std::error_code error;
        const auto text = files::read(options.file, error);
        const bool exists = !error;
        auto contents = exists ? parse(text, options.file) : Contents{};
        fileIsGood = exists && contents.whole;

        // Entries the list's own file has lost, or all of them when it is missing, from the last good one kept.
        if (!fileIsGood)
        {
            std::error_code backupError;
            const auto kept = files::read(backup, backupError);
            if (!backupError)
            {
                auto older = parse(kept, backup);
                int taken = 0;
                for (auto& [key, record] : older.records)
                    taken += contents.records.emplace(key, std::move(record)).second ? 1 : 0;
                for (auto& [identity, entry] : older.seen)
                    contents.seen.emplace(identity, std::move(entry));
                if (!exists)
                    contents.otherMembers = std::move(older.otherMembers);
                if (taken > 0)
                    log("Took " + std::to_string(taken) + " entries of the plugin list from its backup " +
                        paths::toUtf8(backup) + ".");
            }
            changed = exists || !contents.records.empty() || !contents.seen.empty();
        }

        records = std::move(contents.records);
        seen = std::move(contents.seen);
        otherMembers = std::move(contents.otherMembers);
        underway = contents.underway;
    }

    std::string write() const
    {
        std::string text = "{\n\"format\": " + std::to_string(currentFormat) + ",\n";
        text += std::string("\"scanUnderway\": ") + (underway ? "true" : "false") + ",\n";
        for (const auto& member : otherMembers)
            text += json::write(member.name()) + ": " + json::write(member.value) + ",\n";

        text += "\"files\": [\n";
        bool first = true;
        for (const auto& [key, record] : records)
        {
            text += first ? "" : ",\n";
            text += json::write(record.read ? Value(*record.read) : Value(writeFile(record)));
            first = false;
        }

        text += "\n],\n\"seen\": [\n";
        first = true;
        for (const auto& [identity, entry] : seen)
        {
            text += first ? "" : ",\n";
            text += json::write(
                entry.read
                    ? Value(*entry.read)
                    : Value(Object{{"format", identity.format}, {"id", identity.id}, {"firstSeen", entry.firstSeen}}));
            first = false;
        }
        text += "\n]\n}\n";
        return text;
    }

    // The entries for the plugins of which any place is wanted, each with its places in the order of the rule.
    template <typename Wanted>
    std::vector<PluginEntry> entries(Wanted wanted) const
    {
        struct Place
        {
            const PluginDescription* plugin;
            const PluginFile* file;
        };

        std::set<PluginIdentity> identities;
        for (const auto& [key, record] : records)
            if (record.file.outcome == PluginFile::Outcome::found)
                for (const auto& plugin : record.file.plugins)
                    if (wanted(plugin))
                        identities.insert(plugin.identity);

        // Gathered in order of path, which the sort keeps where the rule finds two places alike.
        std::map<PluginIdentity, std::vector<Place>> places;
        for (const auto& [key, record] : records)
            if (record.file.outcome == PluginFile::Outcome::found)
                for (const auto& plugin : record.file.plugins)
                    if (identities.contains(plugin.identity))
                        places[plugin.identity].push_back({&plugin, &record.file});

        const auto native = system::architecture();
        const auto isNative = [native](const PluginFile& file) {
            return file.architectures.empty() ||
                   std::find(file.architectures.begin(), file.architectures.end(), native) != file.architectures.end();
        };

        std::vector<PluginEntry> found;
        for (auto& [identity, those] : places)
        {
            std::stable_sort(those.begin(), those.end(), [&isNative](const Place& a, const Place& b) {
                if (a.file->connected != b.file->connected)
                    return a.file->connected;
                if (isNative(*a.file) != isNative(*b.file))
                    return isNative(*a.file);
                if (const int newer = compareVersions(a.plugin->version, b.plugin->version); newer != 0)
                    return newer > 0;
                return a.file->userFolder && !b.file->userFolder;
            });

            PluginEntry entry;
            entry.plugin = *those.front().plugin;
            for (std::size_t i = 1; i < those.size(); ++i)
                entry.alternates.push_back(*those[i].plugin);
            entry.connected = those.front().file->connected;
            if (const auto first = seen.find(identity); first != seen.end())
                entry.firstSeen = fromMilliseconds(first->second.firstSeen);
            entry.updated = Clock::time_point(std::chrono::duration_cast<Clock::duration>(
                std::chrono::nanoseconds(those.front().file->stamp.modified)));
            found.push_back(std::move(entry));
        }
        return found;
    }

    // Changes one file's record by the function, which gives whether it changed anything.
    template <typename Change>
    void change(Record& record, Change how)
    {
        if (how(record.file))
        {
            record.read.reset();
            changed = true;
        }
    }
};

PluginList::PluginList(Options options) : inner(std::make_unique<Inner>(std::move(options)))
{
    inner->load();
}

PluginList::~PluginList() = default;

std::vector<PluginEntry> PluginList::plugins() const
{
    return inner->entries([](const PluginDescription&) { return true; });
}

std::optional<PluginEntry> PluginList::find(const PluginIdentity& identity) const
{
    auto found = inner->entries([&identity](const PluginDescription& plugin) { return plugin.identity == identity; });
    if (found.empty())
        return std::nullopt;
    return std::move(found.front());
}

std::vector<PluginEntry> PluginList::findByName(std::string_view format, std::string_view maker,
                                                std::string_view name) const
{
    return inner->entries([&](const PluginDescription& plugin) {
        return plugin.identity.format == format && plugin.maker == maker && plugin.name == name;
    });
}

std::vector<PluginFile> PluginList::files() const
{
    std::vector<PluginFile> all;
    for (const auto& [key, record] : inner->records)
        all.push_back(record.file);
    return all;
}

std::optional<PluginFile> PluginList::file(const std::filesystem::path& path) const
{
    const auto found = inner->records.find(keyOf(path));
    if (found == inner->records.end())
        return std::nullopt;
    return found->second.file;
}

bool PluginList::needsScan(const std::filesystem::path& path, const PluginStamp& stamp) const
{
    const auto found = inner->records.find(keyOf(path));
    if (found == inner->records.end())
        return true;
    const auto& record = found->second;
    if (record.file.again || !record.file.complete || record.file.stamp != stamp ||
        record.file.outcome == PluginFile::Outcome::notYetSeen ||
        record.file.outcome == PluginFile::Outcome::notNotarized)
        return true;
    return record.file.outcome == PluginFile::Outcome::changedAfterSigning &&
           differentStarts(inner->computerStarted(), record.computerStarted);
}

void PluginList::record(PluginFile file)
{
    auto& list = *inner;
    const auto now = fromMilliseconds(millisecondsOf(list.now()));
    const auto fill = [&](PluginFailure& failure) {
        failure.date = now;
        failure.programVersion = list.options.programVersion;
        failure.architecture = std::string(system::architecture());
    };

    // Each plugin of the file's format and located at it, and each listed once.
    std::set<PluginIdentity> listed;
    std::vector<PluginDescription> plugins;
    for (auto& plugin : file.plugins)
    {
        plugin.identity.format = file.format;
        plugin.location = file.path;
        if (listed.insert(plugin.identity).second)
            plugins.push_back(std::move(plugin));
    }
    file.plugins = std::move(plugins);

    // One that crashed as it was taken down had been found, and what it did last is what counts.
    for (const auto& aside : file.setAside)
        std::erase_if(file.plugins,
                      [&](const PluginDescription& plugin) { return plugin.identity.id == aside.plugin.identity.id; });

    for (auto& plugin : file.setAside)
    {
        plugin.plugin.identity.format = file.format;
        plugin.plugin.location = file.path;
        fill(plugin.failure);
    }
    if (file.failure)
        fill(*file.failure);

    for (const auto& plugin : file.plugins)
        if (list.seen.emplace(plugin.identity, Seen{millisecondsOf(now), std::nullopt}).second)
            list.changed = true;

    Record record{std::move(file), list.computerStarted(), std::nullopt};
    const auto key = keyOf(record.file.path);
    if (const auto before = list.records.find(key);
        before != list.records.end() && before->second.file == record.file &&
        !differentStarts(before->second.computerStarted, record.computerStarted))
        return;
    list.records.insert_or_assign(key, std::move(record));
    list.changed = true;
}

void PluginList::reachedThrough(const std::filesystem::path& file, const std::filesystem::path& through)
{
    const auto record = inner->records.find(keyOf(file));
    if (record != inner->records.end() && record->second.file.through != through)
    {
        record->second.file.through = through;
        inner->changed = true;
    }
}

void PluginList::folderRead(const std::string& format, const std::filesystem::path& folder,
                            const std::vector<std::filesystem::path>& found,
                            const std::vector<std::filesystem::path>& unreadable)
{
    std::set<std::string> there;
    for (const auto& path : found)
        there.insert(keyOf(path));

    auto& records = inner->records;
    for (auto record = records.begin(); record != records.end();)
    {
        const auto& path = record->second.file.path;
        const auto& through = record->second.file.through;

        if (record->second.file.format != format ||
            (!inside(path, folder) && !(!through.empty() && inside(through, folder))))
            ++record;
        else if (std::any_of(unreadable.begin(), unreadable.end(),
                             [&path](const std::filesystem::path& part) { return inside(path, part); }))
        {
            inner->change(record->second, [](PluginFile& file) { return std::exchange(file.connected, false); });
            ++record;
        }
        else if (!there.contains(record->first))
        {
            record = records.erase(record);
            inner->changed = true;
        }
        else
        {
            inner->change(record->second, [](PluginFile& file) { return !std::exchange(file.connected, true); });
            ++record;
        }
    }
}

void PluginList::folderUnreadable(const std::filesystem::path& folder)
{
    for (auto& [key, record] : inner->records)
        if (inside(record.file.path, folder))
            inner->change(record, [](PluginFile& file) { return std::exchange(file.connected, false); });
}

void PluginList::tryAgain(const std::filesystem::path& path)
{
    if (const auto found = inner->records.find(keyOf(path)); found != inner->records.end())
        inner->change(found->second, [](PluginFile& file) { return !std::exchange(file.again, true); });
}

bool PluginList::scanUnderway() const
{
    return inner->underway;
}

void PluginList::setScanUnderway(bool underway)
{
    if (std::exchange(inner->underway, underway) != underway)
        inner->changed = true;
}

void PluginList::clear()
{
    if (!inner->records.empty())
    {
        inner->records.clear();
        inner->changed = true;
    }
}

bool PluginList::hasChanges() const
{
    return inner->changed;
}

void PluginList::save()
{
    std::error_code error;
    save(error);
    if (error)
        throw std::filesystem::filesystem_error("Can't save the plugin list", inner->options.file, error);
}

void PluginList::save(std::error_code& error)
{
    error.clear();
    auto& list = *inner;
    if (!list.changed)
        return;

    const auto text = list.write();

    // The file before becomes the backup only when it was whole: a damaged one never replaces the last good one.
    if (list.fileIsGood)
    {
        std::error_code ignored;
        std::filesystem::rename(list.options.file, list.backup, ignored);
    }
    files::writeSafely(list.options.file, text, error);
    if (error)
        return;
    list.fileIsGood = true;
    list.changed = false;
}

}
