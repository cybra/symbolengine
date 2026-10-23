#include <symbolengine/plugin/Parameters.h>

#include <symbolengine/core/Text.h>

#include "TextValues.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace symbolengine::plugin
{

namespace detail
{

std::optional<double> leadingNumber(std::string_view typed)
{
    auto text = text::trim(typed);

    bool negative = false;
    if (!text.empty() && (text.front() == '-' || text.front() == '+'))
    {
        negative = text.front() == '-';
        text.remove_prefix(1);
    }

    if (text::equalsIgnoringCaseAscii(text.substr(0, 3), "inf"))
        return negative ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity();

    const auto isDigit = [&](std::size_t i) { return i < text.size() && text[i] >= '0' && text[i] <= '9'; };

    // Rewritten in the JSON form, which text::parseNumber reads the same everywhere: "-" if negative, the whole part
    // or 0, then any fraction and exponent.
    std::string number = negative ? "-" : "";
    std::size_t i = 0;
    while (isDigit(i))
        ++i;
    std::string whole(text.substr(0, i));
    whole.erase(0, std::min(whole.find_first_not_of('0'), whole.size() - 1));
    number += whole.empty() ? "0" : whole;

    bool anyDigits = i > 0;
    if (i < text.size() && text[i] == '.')
    {
        const auto fractionStart = ++i;
        while (isDigit(i))
            ++i;
        if (i > fractionStart)
        {
            number += '.';
            number += text.substr(fractionStart, i - fractionStart);
            anyDigits = true;
        }
    }
    if (!anyDigits)
        return std::nullopt;

    if (i < text.size() && (text[i] == 'e' || text[i] == 'E'))
    {
        auto j = i + 1;
        if (j < text.size() && (text[j] == '+' || text[j] == '-'))
            ++j;
        const auto digitsStart = j;
        while (isDigit(j))
            ++j;
        if (j > digitsStart)
            number += text.substr(i, j - i);
    }

    return text::parseNumber(number);
}

}

namespace
{
std::size_t choiceIndex(double value, std::size_t count)
{
    const auto index = static_cast<long long>(std::llround(value));
    return static_cast<std::size_t>(std::clamp<long long>(index, 0, static_cast<long long>(count) - 1));
}
}

Parameter decibels(std::uint32_t id, std::string name, double minimum, double maximum, double defaultValue)
{
    Parameter parameter{
        .id = id, .name = std::move(name), .minimum = minimum, .maximum = maximum, .defaultValue = defaultValue};
    parameter.toText = [](double value) { return text::formatFixed(value, 1) + " dB"; };
    parameter.fromText = [](std::string_view typed) { return detail::leadingNumber(typed); };
    return parameter;
}

Parameter number(std::uint32_t id, std::string name, double minimum, double maximum, double defaultValue,
                 std::string unit, int decimals)
{
    Parameter parameter{
        .id = id, .name = std::move(name), .minimum = minimum, .maximum = maximum, .defaultValue = defaultValue};
    parameter.toText = [unit, decimals](double value) {
        auto shown = text::formatFixed(value, decimals);
        return unit.empty() ? shown : shown + " " + unit;
    };
    parameter.fromText = [](std::string_view typed) { return detail::leadingNumber(typed); };
    return parameter;
}

Parameter choice(std::uint32_t id, std::string name, std::vector<std::string> choices, std::size_t defaultIndex)
{
    if (choices.empty())
        throw std::logic_error("a choice needs at least one choice");
    if (defaultIndex >= choices.size())
        throw std::logic_error("a choice's default must be one of its choices");

    Parameter parameter{.id = id,
                        .name = std::move(name),
                        .minimum = 0.0,
                        .maximum = static_cast<double>(choices.size() - 1),
                        .defaultValue = static_cast<double>(defaultIndex),
                        .stepped = true};

    parameter.toText = [choices](double value) { return choices[choiceIndex(value, choices.size())]; };
    parameter.fromText = [choices](std::string_view typed) -> std::optional<double> {
        const auto trimmed = text::trim(typed);
        for (std::size_t i = 0; i < choices.size(); ++i)
            if (text::equalsIgnoringCaseAscii(trimmed, choices[i]))
                return static_cast<double>(i);
        return detail::leadingNumber(trimmed);
    };
    return parameter;
}

Parameter toggle(std::uint32_t id, std::string name, bool defaultOn)
{
    Parameter parameter{.id = id,
                        .name = std::move(name),
                        .minimum = 0.0,
                        .maximum = 1.0,
                        .defaultValue = defaultOn ? 1.0 : 0.0,
                        .stepped = true};

    parameter.toText = [](double value) { return std::string(value >= 0.5 ? "On" : "Off"); };
    parameter.fromText = [](std::string_view typed) -> std::optional<double> {
        const auto trimmed = text::trim(typed);
        for (const char* on : {"on", "yes", "true"})
            if (text::equalsIgnoringCaseAscii(trimmed, on))
                return 1.0;
        for (const char* off : {"off", "no", "false"})
            if (text::equalsIgnoringCaseAscii(trimmed, off))
                return 0.0;
        return detail::leadingNumber(trimmed);
    };
    return parameter;
}

Parameter bypass(std::uint32_t id)
{
    auto parameter = toggle(id, "Bypass", false);
    parameter.bypass = true;
    return parameter;
}

}
