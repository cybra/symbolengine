#pragma once

#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

/** Helpers for UTF-8 text held in std::string and std::string_view. */
namespace symbolengine::text
{

/** Whether the text is well-formed UTF-8: no overlong forms, no surrogates and nothing above U+10FFFF. */
bool isValidUtf8(std::string_view text) noexcept;

/** The text with each broken UTF-8 sequence replaced by U+FFFD, one for each maximal broken part, as browsers decode. */
std::string replaceInvalidUtf8(std::string_view text);

/** The text without the spaces, tabs and line breaks at its ends. */
std::string_view trim(std::string_view text) noexcept;

/** The pieces between each separator, empty pieces included, so joining them with it gives the text back. An empty
    separator gives the whole text as one piece. */
std::vector<std::string_view> split(std::string_view text, std::string_view separator);

/** The pieces with the separator between each one. */
template <std::ranges::input_range Range>
std::string join(const Range& pieces, std::string_view separator)
{
    std::string joined;
    bool first = true;
    for (const auto& piece : pieces)
    {
        if (!first)
            joined += separator;
        joined += std::string_view(piece);
        first = false;
    }
    return joined;
}

/** The text with every occurrence of one string replaced by another, from left to right. */
std::string replaceAll(std::string_view text, std::string_view from, std::string_view to);

/** The text with A to Z made lower or upper case and everything else left as it is. */
std::string toLowerAscii(std::string_view text);
std::string toUpperAscii(std::string_view text);

/** Whether two texts are the same when A to Z are compared without case. */
bool equalsIgnoringCaseAscii(std::string_view a, std::string_view b) noexcept;

/** A number as browsers write it: the fewest digits that read back as the same double, in plain form from 1e-7 up to
    1e21 and with an exponent outside that, such as 0.1, 48000, 1.5e-7 and 1e+21. "NaN", "Infinity" and "-Infinity"
    for numbers that aren't finite. The same on every system, whatever its locale. */
std::string formatNumber(double number);

/** A number with a fixed number of decimal places, such as -6.0 or 48.25, rounded to the nearest; one that rounds to
    zero has no minus sign. "NaN", "Infinity" and "-Infinity" for numbers that aren't finite. The same on every system,
    whatever its locale. */
std::string formatFixed(double number, int decimals);

/** A number written as JSON writes one, such as -12, 0.5 or 1e-7, read to the nearest double; nothing for anything
    else, spaces included. The same on every system, whatever its locale. */
std::optional<double> parseNumber(std::string_view text);

}
