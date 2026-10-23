#include "Numbers.h"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <locale.h>
#include <string>
#include <system_error>

#if defined(__APPLE__)
    #include <xlocale.h>
#endif

// Apple's std::to_chars prints doubles only from macOS 13.3 and iOS 16.3, and is refused outright when building for
// anything earlier.
#if defined(__ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__) && __ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__ < 130300
    #define SYMBOLENGINE_PRINT_DOUBLES 1
#elif defined(__ENVIRONMENT_IPHONE_OS_VERSION_MIN_REQUIRED__) && __ENVIRONMENT_IPHONE_OS_VERSION_MIN_REQUIRED__ < 160300
    #define SYMBOLENGINE_PRINT_DOUBLES 1
#else
    #define SYMBOLENGINE_PRINT_DOUBLES 0
#endif

namespace symbolengine::numbers
{

namespace
{
// printf and strtod follow the program's locale, which a toolkit such as GTK sets from the user's, giving "1,5"
// for 1.5 in much of Europe. Text written for files and other programs always uses the C locale.
#if defined(_WIN32)
_locale_t cLocale()
{
    static const _locale_t locale = _create_locale(LC_ALL, "C");
    return locale;
}

struct CLocaleScope
{
};

int printScientific(char* buffer, std::size_t size, double number, int digits)
{
    return _snprintf_s_l(buffer, size, _TRUNCATE, "%.*e", cLocale(), digits - 1, number);
}

double readTerminated(const char* text)
{
    return _strtod_l(text, nullptr, cLocale());
}

int printFixed(char* buffer, std::size_t size, double number, int decimals)
{
    return _snprintf_s_l(buffer, size, _TRUNCATE, "%.*f", cLocale(), decimals, number);
}
#else
locale_t cLocale()
{
    static const locale_t locale = newlocale(LC_ALL_MASK, "C", nullptr);
    return locale;
}

struct CLocaleScope
{
    CLocaleScope() : previous(uselocale(cLocale())) {}
    ~CLocaleScope() { uselocale(previous); }
    CLocaleScope(const CLocaleScope&) = delete;
    CLocaleScope& operator=(const CLocaleScope&) = delete;

    locale_t previous;
};

int printScientific(char* buffer, std::size_t size, double number, int digits)
{
    return std::snprintf(buffer, size, "%.*e", digits - 1, number);
}

double readTerminated(const char* text)
{
    return std::strtod(text, nullptr);
}

int printFixed(char* buffer, std::size_t size, double number, int decimals)
{
    return std::snprintf(buffer, size, "%.*f", decimals, number);
}
#endif

}

std::string fixed(double number, int decimals)
{
    [[maybe_unused]] CLocaleScope scope;   // empty on Windows, whose functions take the locale

    // The largest double has 309 digits before the point, and no more than 17 after it are ever meaningful.
    char buffer[400];
    const int length = printFixed(buffer, sizeof buffer, number, decimals < 0 ? 0 : decimals > 17 ? 17 : decimals);
    return length > 0 ? std::string(buffer, static_cast<std::size_t>(length)) : std::string();
}

Decimal fromScientific(const char* text, std::size_t length)
{
    Decimal decimal;
    std::size_t i = 0;

    for (; i < length && text[i] != 'e'; ++i)
        if (text[i] != '.' && decimal.count < static_cast<int>(sizeof decimal.digits))
            decimal.digits[decimal.count++] = text[i];

    while (decimal.count > 1 && decimal.digits[decimal.count - 1] == '0')
        --decimal.count;

    std::size_t exponentStart = i + 1;
    if (exponentStart < length && text[exponentStart] == '+')
        ++exponentStart;

    int exponent = 0;
    if (exponentStart < length && std::from_chars(text + exponentStart, text + length, exponent).ec != std::errc())
        exponent = 0;

    decimal.exponent = exponent + 1;
    return decimal;
}

Decimal shortestByPrinting(double number)
{
    [[maybe_unused]] CLocaleScope scope;   // empty on Windows, whose functions take the locale
    char buffer[48];

    // At a power of two the gap to the double below is half the gap above, so the decimal nearest it with a given
    // number of digits can lie below, just out of reach, while the next one up with as many digits still reads back.
    int binaryExponent = 0;
    const bool powerOfTwo = std::frexp(number, &binaryExponent) == 0.5;

    for (int digits = 1;; ++digits)
    {
        const int length = printScientific(buffer, sizeof buffer, number, digits);
        if (digits >= 17 || readTerminated(buffer) == number)
            return fromScientific(buffer, static_cast<std::size_t>(length));

        if (powerOfTwo)
        {
            // The printed digits plus one in the last place, carried as far as it goes.
            std::string up;
            std::size_t i = 0;
            for (; i < static_cast<std::size_t>(length) && buffer[i] != 'e'; ++i)
                if (buffer[i] != '.')
                    up += buffer[i];

            int exponent = std::atoi(buffer + i + 1);
            auto place = up.size();
            while (place > 0 && up[place - 1] == '9')
                up[--place] = '0';
            if (place > 0)
                ++up[place - 1];
            else
            {
                up.insert(up.begin(), '1');
                up.pop_back();
                ++exponent;
            }

            const auto next = up.substr(0, 1) + "." + up.substr(1) + "e" + std::to_string(exponent);
            if (readTerminated(next.c_str()) == number)
                return fromScientific(next.c_str(), next.size());
        }
    }
}

Decimal shortest(double number)
{
#if SYMBOLENGINE_PRINT_DOUBLES
    return shortestByPrinting(number);
#else
    char buffer[48];
    const auto result = std::to_chars(buffer, buffer + sizeof buffer, number, std::chars_format::scientific);
    return fromScientific(buffer, static_cast<std::size_t>(result.ptr - buffer));
#endif
}

std::size_t jsonNumberLength(std::string_view text, bool* integral) noexcept
{
    const auto digit = [&](std::size_t i) { return i < text.size() && text[i] >= '0' && text[i] <= '9'; };

    std::size_t i = 0;
    if (i < text.size() && text[i] == '-')
        ++i;

    if (!digit(i))
        return 0;
    if (text[i] == '0')
        ++i;
    else
        while (digit(i))
            ++i;

    bool whole = true;

    if (i < text.size() && text[i] == '.')
    {
        if (!digit(i + 1))
            return 0;
        i += 1;
        while (digit(i))
            ++i;
        whole = false;
    }

    if (i < text.size() && (text[i] == 'e' || text[i] == 'E'))
    {
        std::size_t j = i + 1;
        if (j < text.size() && (text[j] == '+' || text[j] == '-'))
            ++j;
        if (!digit(j))
            return 0;
        while (digit(j))
            ++j;
        i = j;
        whole = false;
    }

    if (integral != nullptr)
        *integral = whole;
    return i;
}

double read(std::string_view checkedNumber)
{
    char small[64];
    std::string large;
    const char* terminated = small;

    if (checkedNumber.size() < sizeof small)
    {
        std::memcpy(small, checkedNumber.data(), checkedNumber.size());
        small[checkedNumber.size()] = '\0';
    }
    else
    {
        large.assign(checkedNumber);
        terminated = large.c_str();
    }

    [[maybe_unused]] CLocaleScope scope;   // empty on Windows, whose functions take the locale
    return readTerminated(terminated);
}

}
