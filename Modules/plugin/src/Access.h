#pragma once

#include <symbolengine/plugin/Plugin.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

namespace symbolengine::plugin::detail
{

/** The framework's way into a plugin's parameters and host link. */
struct Access
{
    static std::size_t count(const Plugin& plugin) noexcept { return plugin.definitions.size(); }

    static std::optional<std::size_t> index(const Plugin& plugin, std::uint32_t id) noexcept
    {
        const auto found = plugin.indexOf.find(id);
        if (found == plugin.indexOf.end())
            return std::nullopt;
        return found->second;
    }

    static double value(const Plugin& plugin, std::size_t index) noexcept
    {
        return plugin.values[index].load(std::memory_order_relaxed);
    }

    /** Sets a value as a host may: held within the parameter's range, a stepped one rounded, NaN ignored. */
    static void set(Plugin& plugin, std::size_t index, double value) noexcept
    {
        if (std::isnan(value))
            return;
        const auto& parameter = plugin.definitions[index];
        value = std::clamp(value, parameter.minimum, parameter.maximum);
        if (parameter.stepped)
            value = std::round(value);
        plugin.values[index].store(value, std::memory_order_relaxed);
    }

    static void attach(Plugin& plugin, HostLink* host) noexcept { plugin.host = host; }

    static void setOffline(Plugin& plugin, bool offline) noexcept
    {
        plugin.offline.store(offline, std::memory_order_relaxed);
    }

    /** Sets the layouts the host chose, while the plugin isn't prepared. */
    static void setLayouts(Plugin& plugin, Layouts layouts) { plugin.current = std::move(layouts); }

    /** This instance's editor size in points, or 0 by 0 until it has one. */
    static std::pair<std::uint32_t, std::uint32_t> editorSize(const Plugin& plugin) noexcept
    {
        return {plugin.editorWidth, plugin.editorHeight};
    }

    static void keepEditorSize(Plugin& plugin, std::uint32_t width, std::uint32_t height) noexcept
    {
        plugin.editorWidth = width;
        plugin.editorHeight = height;
    }
};

}
