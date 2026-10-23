#pragma once

#include "Value.h"

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

/** Reading and writing JSON (RFC 8259) as Values, in UTF-8. */
namespace symbolengine::json
{

enum class Layout
{
    compact,   // no spaces or line breaks
    pretty     // each member and item on its own line, indented by two spaces
};

/** Writes a value as JSON. Numbers are written as browsers write them, in the fewest digits that read back as the
    same number, and a real that isn't finite is written as null, as browsers do. A whole real from 2^53 up, where
    doubles no longer hold every whole number, is written with an exponent, so that it reads back as the same real and
    not as a nearby integer. Text that isn't valid UTF-8 has each broken sequence written as U+FFFD, so the result is
    always valid JSON. */
std::string write(const Value& value, Layout layout = Layout::compact);

/** Thrown when text isn't JSON, saying why and where: the line and column, from 1, with the column counted in bytes. */
class ParseError : public std::runtime_error
{
public:
    ParseError(const std::string& reason, std::size_t line, std::size_t column);

    const std::string& reason() const noexcept { return why; }
    std::size_t line() const noexcept { return where.line; }
    std::size_t column() const noexcept { return where.column; }

private:
    std::string why;
    struct
    {
        std::size_t line, column;
    } where;
};

/** The deepest arrays and objects may nest, so that hostile text can't exhaust the stack. */
inline constexpr std::size_t maximumDepth = 512;

/** Reads JSON text, throwing ParseError if it isn't JSON. A byte order mark at the start is skipped. A number is read
    as an integer when it is whole, written without a fraction or exponent, and fits in 64 bits, and as a real
    otherwise. A name given twice in one object keeps the place of its first and the value of its last, as browsers
    do, and an escaped surrogate that isn't half of a pair reads as U+FFFD. */
Value parse(std::string_view text);

/** Reads JSON text, or gives nothing if it isn't JSON. */
std::optional<Value> tryParse(std::string_view text);

}
