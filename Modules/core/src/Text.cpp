#include "symbolengine/core/Text.h"

#include "Numbers.h"
#include "Utf8.h"

#include <cmath>
#include <cstdlib>

namespace symbolengine::text
{

bool isValidUtf8(std::string_view text) noexcept
{
    return utf8::firstInvalid(text) == text.size();
}

std::string replaceInvalidUtf8(std::string_view text)
{
    std::string replaced;
    utf8::appendReplacingInvalid(replaced, text);
    return replaced;
}

std::string_view trim(std::string_view text) noexcept
{
    constexpr std::string_view space = " \t\r\n\f\v";
    const auto first = text.find_first_not_of(space);
    if (first == std::string_view::npos)
        return {};
    const auto last = text.find_last_not_of(space);
    return text.substr(first, last - first + 1);
}

std::vector<std::string_view> split(std::string_view text, std::string_view separator)
{
    std::vector<std::string_view> pieces;

    if (separator.empty())
    {
        pieces.push_back(text);
        return pieces;
    }

    std::size_t start = 0;
    for (;;)
    {
        const auto found = text.find(separator, start);
        if (found == std::string_view::npos)
        {
            pieces.push_back(text.substr(start));
            return pieces;
        }
        pieces.push_back(text.substr(start, found - start));
        start = found + separator.size();
    }
}

std::string replaceAll(std::string_view text, std::string_view from, std::string_view to)
{
    if (from.empty())
        return std::string(text);

    std::string replaced;
    std::size_t start = 0;
    for (;;)
    {
        const auto found = text.find(from, start);
        if (found == std::string_view::npos)
        {
            replaced += text.substr(start);
            return replaced;
        }
        replaced += text.substr(start, found - start);
        replaced += to;
        start = found + from.size();
    }
}

namespace
{
constexpr char lowerAscii(char c) noexcept
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

constexpr char upperAscii(char c) noexcept
{
    return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
}
}

std::string toLowerAscii(std::string_view text)
{
    std::string lower(text);
    for (auto& c : lower)
        c = lowerAscii(c);
    return lower;
}

std::string toUpperAscii(std::string_view text)
{
    std::string upper(text);
    for (auto& c : upper)
        c = upperAscii(c);
    return upper;
}

bool equalsIgnoringCaseAscii(std::string_view a, std::string_view b) noexcept
{
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (lowerAscii(a[i]) != lowerAscii(b[i]))
            return false;
    return true;
}

std::string formatNumber(double number)
{
    if (std::isnan(number))
        return "NaN";
    if (std::isinf(number))
        return number > 0 ? "Infinity" : "-Infinity";
    if (number == 0)
        return "0";

    // Laid out as ECMAScript's Number::toString lays out the digits d of a number 0.d times 10 to the power n.
    const auto decimal = numbers::shortest(std::fabs(number));
    const std::string_view digits(decimal.digits, static_cast<std::size_t>(decimal.count));
    const int k = decimal.count;
    const int n = decimal.exponent;

    std::string text;
    if (number < 0)
        text += '-';

    if (k <= n && n <= 21)
    {
        text += digits;
        text.append(static_cast<std::size_t>(n - k), '0');
    }
    else if (0 < n && n <= 21)
    {
        text += digits.substr(0, static_cast<std::size_t>(n));
        text += '.';
        text += digits.substr(static_cast<std::size_t>(n));
    }
    else if (-6 < n && n <= 0)
    {
        text += "0.";
        text.append(static_cast<std::size_t>(-n), '0');
        text += digits;
    }
    else
    {
        text += digits[0];
        if (k > 1)
        {
            text += '.';
            text += digits.substr(1);
        }
        text += n - 1 >= 0 ? "e+" : "e-";
        text += std::to_string(std::abs(n - 1));
    }

    return text;
}

std::string formatFixed(double number, int decimals)
{
    if (!std::isfinite(number))
        return formatNumber(number);

    auto text = numbers::fixed(number, decimals);
    if (text.starts_with('-') && text.find_first_not_of("-0.") == std::string::npos)
        text.erase(0, 1);
    return text;
}

std::optional<double> parseNumber(std::string_view text)
{
    if (text.empty() || numbers::jsonNumberLength(text) != text.size())
        return std::nullopt;
    return numbers::read(text);
}

}
