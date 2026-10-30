#pragma once

#include "symbolengine/host/PluginList.h"

#include <symbolengine/core/Value.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

// Plugins and failures as JSON, as the list keeps them and the scan's workers send them, and the reading that says why
// an entry or a message can't be used.
namespace symbolengine::pluginJson
{

using Clock = std::chrono::system_clock;

inline std::int64_t millisecondsOf(Clock::time_point time)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count();
}

// A time read from a file, held to what the system clock can hold, since a damaged file can say anything.
inline Clock::time_point fromMilliseconds(std::int64_t count)
{
    using std::chrono::milliseconds;
    constexpr auto most = std::chrono::duration_cast<milliseconds>(Clock::duration::max()).count();
    constexpr auto least = std::chrono::duration_cast<milliseconds>(Clock::duration::min()).count();
    return Clock::time_point(std::chrono::duration_cast<Clock::duration>(milliseconds(std::clamp(count, least, most))));
}

template <typename Enum>
struct Named
{
    Enum value;
    std::string_view name;
};

inline constexpr Named<PluginFile::Outcome> outcomes[] = {
    {PluginFile::Outcome::found, "found"},
    {PluginFile::Outcome::nothing, "nothing"},
    {PluginFile::Outcome::otherArchitecture, "otherArchitecture"},
    {PluginFile::Outcome::changedAfterSigning, "changedAfterSigning"},
    {PluginFile::Outcome::notNotarized, "notNotarized"},
    {PluginFile::Outcome::setAside, "setAside"},
    {PluginFile::Outcome::notYetSeen, "notYetSeen"},
};

inline constexpr Named<PluginFailure::Kind> failureKinds[] = {
    {PluginFailure::Kind::crashed, "crashed"},
    {PluginFailure::Kind::timedOut, "timedOut"},
    {PluginFailure::Kind::noAnswer, "noAnswer"},
    {PluginFailure::Kind::refused, "refused"},
    {PluginFailure::Kind::codeSignature, "codeSignature"},
};

inline constexpr Named<PluginKind> pluginKinds[] = {
    {PluginKind::effect, "effect"},
    {PluginKind::instrument, "instrument"},
    {PluginKind::other, "other"},
};

template <typename Enum, std::size_t count>
inline std::string nameOf(Enum value, const Named<Enum> (&names)[count])
{
    for (const auto& named : names)
        if (named.value == value)
            return std::string(named.name);
    return std::string(names[0].name);
}

template <typename Enum, std::size_t count>
inline Enum valueOf(const Value& value, const Named<Enum> (&names)[count])
{
    const auto& name = value.asString();
    for (const auto& named : names)
        if (named.name == name)
            return named.value;
    throw TypeError("it says \"" + name + "\", which this version doesn't know");
}

// Reading an entry's members: one that must be there, and those that may be missing, each of the kind it must be, or
// TypeError says why the entry can't be read.
inline const Value& required(const Object& object, std::string_view name)
{
    const auto* value = object.find(name);
    if (value == nullptr)
        throw TypeError("it has no " + std::string(name));
    return *value;
}

inline std::string textOr(const Object& object, std::string_view name)
{
    const auto* value = object.find(name);
    return value != nullptr ? value->asString() : std::string();
}

inline std::int64_t numberOr(const Object& object, std::string_view name)
{
    const auto* value = object.find(name);
    return value != nullptr ? value->asInt() : 0;
}

inline bool flagOr(const Object& object, std::string_view name, bool fallback)
{
    const auto* value = object.find(name);
    return value != nullptr ? value->asBool() : fallback;
}

inline Object writeFailure(const PluginFailure& failure)
{
    return Object{{"kind", nameOf(failure.kind, failureKinds)},
                  {"step", failure.step},
                  {"code", failure.code},
                  {"detail", failure.detail},
                  {"date", millisecondsOf(failure.date)},
                  {"programVersion", failure.programVersion},
                  {"architecture", failure.architecture}};
}

inline PluginFailure readFailure(const Value& value)
{
    const auto& object = value.asObject();
    PluginFailure failure;
    failure.kind = valueOf(required(object, "kind"), failureKinds);
    failure.step = textOr(object, "step");
    failure.code = numberOr(object, "code");
    failure.detail = textOr(object, "detail");
    failure.date = fromMilliseconds(numberOr(object, "date"));
    failure.programVersion = textOr(object, "programVersion");
    failure.architecture = textOr(object, "architecture");
    return failure;
}

// A plugin, its format and location being its file's.
inline Object writePlugin(const PluginDescription& plugin)
{
    Array layouts;
    for (const auto& layout : plugin.layouts)
        layouts.push_back(Array{layout.inputs, layout.outputs});
    return Object{{"id", plugin.identity.id},
                  {"name", plugin.name},
                  {"maker", plugin.maker},
                  {"version", plugin.version},
                  {"kind", nameOf(plugin.kind, pluginKinds)},
                  {"layouts", std::move(layouts)},
                  {"editor", plugin.editor},
                  {"sdk", plugin.sdk},
                  {"categories", plugin.categories}};
}

inline PluginDescription readPlugin(const Value& value, const std::string& format,
                                    const std::filesystem::path& location)
{
    const auto& object = value.asObject();
    PluginDescription plugin;
    plugin.identity = {format, required(object, "id").asString()};
    if (plugin.identity.id.empty())
        throw TypeError("a plugin in it has no identity");
    plugin.location = location;
    plugin.name = textOr(object, "name");
    plugin.maker = textOr(object, "maker");
    plugin.version = textOr(object, "version");
    if (const auto* kind = object.find("kind"))
        plugin.kind = valueOf(*kind, pluginKinds);
    if (const auto* layouts = object.find("layouts"))
        for (const auto& layout : layouts->asArray())
        {
            const auto& channels = layout.asArray();
            if (channels.size() != 2)
                throw TypeError("a layout of a plugin in it isn't two numbers");
            const auto inputs = channels[0].asInt();
            const auto outputs = channels[1].asInt();
            if (inputs < 0 || outputs < 0 || inputs > 1024 || outputs > 1024)
                throw TypeError("a layout of a plugin in it has a number of channels no plugin has");
            plugin.layouts.push_back({static_cast<int>(inputs), static_cast<int>(outputs)});
        }
    plugin.editor = flagOr(object, "editor", false);
    plugin.sdk = textOr(object, "sdk");
    plugin.categories = textOr(object, "categories");
    return plugin;
}

}
