#include "Numbers.h"

#include <symbolengine/core/Json.h>
#include <symbolengine/core/Text.h>

#include <catch2/catch_test_macros.hpp>

#include <clocale>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <random>
#include <string>
#include <utility>
#include <vector>

// The reference, in ToCharsReference.cpp.
symbolengine::numbers::Decimal shortestByToChars(double number);

using namespace symbolengine;

namespace
{
std::string asText(const numbers::Decimal& decimal)
{
    return std::string(decimal.digits, static_cast<std::size_t>(decimal.count)) + "e" +
           std::to_string(decimal.exponent);
}

double fromBits(std::uint64_t bits)
{
    double number = 0;
    std::memcpy(&number, &bits, sizeof number);
    return number;
}
}

TEST_CASE("Numbers are written as browsers write them")
{
    // Each as JavaScript's String(number) gives it.
    const std::pair<double, const char*> expected[] = {
        {0.0, "0"},
        {-0.0, "0"},
        {1.0, "1"},
        {-1.0, "-1"},
        {100.0, "100"},
        {48000.0, "48000"},
        {44100.5, "44100.5"},
        {0.1, "0.1"},
        {0.1 + 0.2, "0.30000000000000004"},
        {0.1 + 0.7, "0.7999999999999999"},
        {1.0 / 3.0, "0.3333333333333333"},
        {2.0 / 3.0, "0.6666666666666666"},
        {1234.5678, "1234.5678"},
        {1e20, "100000000000000000000"},
        {123456789012345680000.0, "123456789012345680000"},
        {1e21, "1e+21"},
        {0.000001, "0.000001"},
        {-0.0000012, "-0.0000012"},
        {1e-7, "1e-7"},
        {1.5e-7, "1.5e-7"},
        {5e-324, "5e-324"},
        {2.2250738585072014e-308, "2.2250738585072014e-308"},
        {1.7976931348623157e308, "1.7976931348623157e+308"},
        {9007199254740993.0, "9007199254740992"},
        {std::numeric_limits<double>::quiet_NaN(), "NaN"},
        {std::numeric_limits<double>::infinity(), "Infinity"},
        {-std::numeric_limits<double>::infinity(), "-Infinity"},
    };

    for (const auto& [number, text] : expected)
    {
        INFO(text);
        CHECK(text::formatNumber(number) == text);
    }
}

TEST_CASE("Numbers are written with fixed decimal places")
{
    CHECK(text::formatFixed(-6.0, 1) == "-6.0");
    CHECK(text::formatFixed(1.26, 1) == "1.3");
    CHECK(text::formatFixed(48000.0, 0) == "48000");
    CHECK(text::formatFixed(0.0, 2) == "0.00");
    CHECK(text::formatFixed(-0.04, 1) == "0.0");
    CHECK(text::formatFixed(-1e-10, 3) == "0.000");
    CHECK(text::formatFixed(-0.06, 1) == "-0.1");
    CHECK(text::formatFixed(1.7976931348623157e308, 0).size() == 309);
    CHECK(text::formatFixed(std::numeric_limits<double>::quiet_NaN(), 1) == "NaN");
    CHECK(text::formatFixed(-std::numeric_limits<double>::infinity(), 1) == "-Infinity");
}

TEST_CASE("Numbers are read only in the JSON form")
{
    CHECK(text::parseNumber("0") == 0.0);
    CHECK(text::parseNumber("-12") == -12.0);
    CHECK(text::parseNumber("1.5e3") == 1500.0);
    CHECK(text::parseNumber("1E-2") == 0.01);
    CHECK(text::parseNumber("0.1") == 0.1);
    CHECK(text::parseNumber(std::string(400, '1') + "e-400") == 1.0 / 9.0);

    for (const char* wrong : {"", " 1", "1 ", "+1", "01", ".5", "1.", "0x10", "inf", "nan", "1e", "--1", "1,5"})
    {
        INFO(wrong);
        CHECK_FALSE(text::parseNumber(wrong).has_value());
    }
}

TEST_CASE("Every double reads back from the text written for it")
{
    std::mt19937_64 random(1);
    for (int i = 0; i < 200000; ++i)
    {
        const double number = fromBits(random());
        if (!std::isfinite(number))
            continue;
        const auto written = text::formatNumber(number);
        INFO(written);
        const auto read = text::parseNumber(written);
        REQUIRE(read.has_value());
        CHECK((std::memcmp(&*read, &number, sizeof number) == 0 || number == 0));
    }
}

TEST_CASE("Printing finds the same shortest digits as std::to_chars")
{
    // Powers of two, where the gap below a double is half the gap above, are where a search by length goes wrong.
    std::vector<double> numbers;
    for (int power = -1074; power <= 1023; ++power)
        numbers.push_back(std::ldexp(1.0, power));
    numbers.push_back(std::numeric_limits<double>::max());
    numbers.push_back(std::numeric_limits<double>::denorm_min());
    numbers.push_back(std::numeric_limits<double>::min());

    std::mt19937_64 random(2);
    for (int i = 0; i < 200000; ++i)
    {
        const double number = std::fabs(fromBits(random()));
        if (std::isfinite(number) && number != 0)
            numbers.push_back(number);
    }

    std::size_t compared = 0;
    for (const double number : numbers)
    {
        const auto reference = asText(shortestByToChars(number));
        const auto printed = asText(numbers::shortestByPrinting(number));
        const auto used = asText(numbers::shortest(number));
        if (printed != reference || used != reference)
        {
            INFO(number);
            CHECK(printed == reference);
            CHECK(used == reference);
        }
        ++compared;
    }
    CHECK(compared > 200000);
}

TEST_CASE("Numbers are written and read the same where the locale uses a decimal comma")
{
    bool found = false;
    for (const char* name : {"de_DE.UTF-8", "de_DE.utf8", "de_DE", "fr_FR.UTF-8", "de-DE", "German_Germany.1252"})
    {
        if (std::setlocale(LC_ALL, name) != nullptr)
        {
            found = true;
            break;
        }
    }
    if (!found)
        SKIP("this system has no locale with a decimal comma");

    // The locale really does write a comma, so this test can fail.
    char probe[16];
    std::snprintf(probe, sizeof probe, "%.1f", 1.5);
    CHECK(std::string(probe) == "1,5");

    CHECK(text::formatNumber(1.5) == "1.5");
    CHECK(text::formatNumber(0.30000000000000004) == "0.30000000000000004");
    CHECK(text::formatFixed(-6.25, 2) == "-6.25");
    CHECK(text::parseNumber("1.5") == 1.5);
    CHECK(json::parse("[0.25]")[0].asNumber() == 0.25);
    CHECK(json::write(0.25) == "0.25");

    std::setlocale(LC_ALL, "C");
}
