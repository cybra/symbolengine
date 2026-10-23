#pragma once

#include <cstddef>
#include <string_view>

// Converting doubles to and from text the same way on every system and in every locale.
namespace symbolengine::numbers
{

/** A positive, finite double's shortest decimal: the fewest digits that read back as the same double, nearest it when
    several would, so that the double is 0.digits times 10 to the power exponent. */
struct Decimal
{
    char digits[20];
    int count = 0;
    int exponent = 0;
};

/** The shortest decimal: from std::to_chars where the system can print doubles with it, and otherwise by printing. */
Decimal shortest(double number);

/** The shortest decimal found by printing with the C library, for Macs before macOS 13.3, whose std::to_chars can't
    print doubles. It gives the same digits as std::to_chars. */
Decimal shortestByPrinting(double number);

/** Reads d.ddde-x, as printf's %e and std::to_chars's scientific form write it. */
Decimal fromScientific(const char* text, std::size_t length);

/** How many bytes at the start of the text are a number in the JSON form, -?(0|[1-9][0-9]*)(.[0-9]+)?([eE][+-]?[0-9]+)?,
    and 0 if there isn't one there. integral says whether it has neither a fraction nor an exponent. */
std::size_t jsonNumberLength(std::string_view text, bool* integral = nullptr) noexcept;

/** The nearest double to a number written in the JSON form, which the caller has checked. */
double read(std::string_view checkedNumber);

}
