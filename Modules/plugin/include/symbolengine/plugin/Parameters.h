#pragma once

#include "Plugin.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
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

/** How a parameter's units are spread over the 0 to 1 that hosts see, for a parameter that hosts record from 0 to 1,
    as plugins already released with such parameters have them. Copied freely and used on any thread. */
class Scale
{
public:
    /** Evenly from minimum to maximum. */
    static Scale linear(double minimum, double maximum);

    /** Evenly by ratio, as frequency and Q are heard: each doubling takes the same travel. Both ends above 0. */
    static Scale ratio(double minimum, double maximum);

    /** The units at a position from 0 to 1, held to that range. */
    double toUnits(double position) const noexcept;

    /** The position from 0 to 1 of a number of units, held to the scale's range. */
    double toPosition(double units) const noexcept;

    double minimum() const noexcept { return low; }
    double maximum() const noexcept { return high; }

private:
    enum class Kind
    {
        linear,
        ratio
    };

    Scale(Kind shape, double from, double to) noexcept : kind(shape), low(from), high(to) {}

    Kind kind;
    double low, high;
};

/** A parameter hosts see and record from 0 to 1, spread over its units by a scale, and shown and typed in those units:
    by toText, and by fromText or else the number typed. The plugin reads its units with scale.toUnits(value.get()). */
Parameter scaled(std::uint32_t id, std::string name, Scale scale, double defaultUnits,
                 std::function<std::string(double units)> toText,
                 std::function<std::optional<double>(std::string_view typed)> fromText = {});

}
