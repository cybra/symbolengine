#pragma once

#include "Plugin.h"

#include <string>
#include <vector>

/** Parameters of the usual kinds, each shown and typed as people expect. */
namespace symbolengine::plugin
{

/** A level in decibels, shown to one decimal place, such as "-6.0 dB"; typed with or without "dB". */
Parameter decibels(std::uint32_t id, std::string name, double minimum, double maximum, double defaultValue);

/** A number with a unit, shown to a number of decimal places, such as "250 Hz" or "12.5 ms". */
Parameter number(std::uint32_t id, std::string name, double minimum, double maximum, double defaultValue,
                 std::string unit = {}, int decimals = 2);

/** One of several choices, by its index from 0, shown and typed by its name. */
Parameter choice(std::uint32_t id, std::string name, std::vector<std::string> choices, std::size_t defaultIndex);

/** A switch, 1 for on and 0 for off, shown as "On" or "Off". */
Parameter toggle(std::uint32_t id, std::string name, bool defaultOn);

/** The plugin's bypass, which hosts show as their own. */
Parameter bypass(std::uint32_t id);

}
