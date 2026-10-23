// Built for macOS 13.3 or later even when the rest is built for earlier, so std::to_chars can be the reference the
// library's own printing is checked against.

#include "Numbers.h"

#include <charconv>

symbolengine::numbers::Decimal shortestByToChars(double number)
{
    char buffer[48];
    const auto result = std::to_chars(buffer, buffer + sizeof buffer, number, std::chars_format::scientific);
    return symbolengine::numbers::fromScientific(buffer, static_cast<std::size_t>(result.ptr - buffer));
}
