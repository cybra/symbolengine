#include <symbolengine/core/Json.h>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstring>
#include <limits>
#include <random>
#include <string>

using namespace symbolengine;

namespace
{
// Where parsing the text fails, as "line:column reason".
std::string failure(std::string_view text)
{
    try
    {
        json::parse(text);
        return "parsed";
    }
    catch (const json::ParseError& error)
    {
        return std::to_string(error.line()) + ":" + std::to_string(error.column()) + " " + error.reason();
    }
}
}

TEST_CASE("JSON reads every kind of value")
{
    const auto value = json::parse(R"( {"null": null, "yes": true, "no": false, "int": -12, "real": 0.5,
                                        "text": "a\"b\\c\/d\b\f\n\r\t", "list": [1, [2, []], {}]} )");
    CHECK(value["null"].isNull());
    CHECK(value["yes"].asBool());
    CHECK_FALSE(value["no"].asBool());
    CHECK(value["int"].isInteger());
    CHECK(value["int"].asInt() == -12);
    CHECK(value["real"].isReal());
    CHECK(value["real"].asNumber() == 0.5);
    CHECK(value["text"].asString() == "a\"b\\c/d\b\f\n\r\t");
    CHECK(value["list"][1][0].asInt() == 2);
    CHECK(value["list"][1][1].isArray());
    CHECK(value["list"][2].isObject());
}

TEST_CASE("JSON numbers become integers when whole and in range")
{
    CHECK(json::parse("9223372036854775807").asInt() == std::numeric_limits<std::int64_t>::max());
    CHECK(json::parse("-9223372036854775808").asInt() == std::numeric_limits<std::int64_t>::min());
    CHECK(json::parse("9223372036854775808").isReal());
    CHECK(json::parse("1.0").isReal());
    CHECK(json::parse("1e2").isReal());
    CHECK(json::parse("1e2").asNumber() == 100.0);
    CHECK(json::parse("0").isInteger());

    const auto negativeZero = json::parse("-0");
    CHECK(negativeZero.isReal());
    CHECK(std::signbit(negativeZero.asNumber()));

    CHECK(json::parse("0.1").asNumber() == 0.1);
    CHECK(json::parse("2.2250738585072014e-308").asNumber() == 2.2250738585072014e-308);
    CHECK(json::parse("1e400").asNumber() == std::numeric_limits<double>::infinity());
}

TEST_CASE("JSON escapes decode to UTF-8")
{
    CHECK(json::parse(R"("\u00e9")").asString() == "\xC3\xA9");
    CHECK(json::parse(R"("\u20AC")").asString() == "\xE2\x82\xAC");
    CHECK(json::parse(R"("\ud83d\ude00")").asString() == "\xF0\x9F\x98\x80");
    CHECK(json::parse(R"("\u0000")").asString() == std::string(1, '\0'));

    // A surrogate without its other half reads as U+FFFD, and what follows it is still read.
    CHECK(json::parse(R"("\ud83dx")").asString() == "\xEF\xBF\xBDx");
    CHECK(json::parse(R"("\ude00")").asString() == "\xEF\xBF\xBD");
    CHECK(json::parse(R"("\ud83d\u0041")").asString() == std::string("\xEF\xBF\xBD") + "A");

    CHECK(json::parse("\"caf\xC3\xA9 \xF0\x9F\x8E\xA7\"").asString() == "caf\xC3\xA9 \xF0\x9F\x8E\xA7");
}

TEST_CASE("JSON keeps a repeated name's first place and last value")
{
    const auto value = json::parse(R"({"a": 1, "b": 2, "a": 3})");
    CHECK(json::write(value) == R"({"a":3,"b":2})");
}

TEST_CASE("JSON skips a byte order mark")
{
    CHECK(json::parse("\xEF\xBB\xBF{\"a\": 1}")["a"].asInt() == 1);
}

TEST_CASE("JSON that is wrong is refused, saying where")
{
    CHECK(failure("") == "1:1 the text ends where a value should be");
    CHECK(failure("{\"a\": 1,\n \"b\": }") == "2:7 a value can't start with this character");
    CHECK(failure("[1, 2,]") == "1:7 a value can't start with this character");
    CHECK(failure("{\"a\": 1,}") == "1:9 expected a name in double quotes");
    CHECK(failure("[1 2]") == "1:4 expected a comma or a closing bracket");
    CHECK(failure("{\"a\" 1}") == "1:6 expected a colon after the name");
    CHECK(failure("01") == "1:2 there is more after the value");
    CHECK(failure("1.") == "1:1 a number is written wrongly");
    CHECK(failure("-") == "1:1 a number is written wrongly");
    CHECK(failure(".5") == "1:1 a value can't start with this character");
    CHECK(failure("+1") == "1:1 a value can't start with this character");
    CHECK(failure("NaN") == "1:1 a value can't start with this character");
    CHECK(failure("tru") == "1:1 a value can't start with this character");
    CHECK(failure("\"tab\there\"") == "1:5 a control character in a string must be escaped");
    CHECK(failure("\"\\x\"") == "1:3 a backslash must be followed by an escape");
    CHECK(failure("\"\\u12g4\"") == "1:6 \\u must be followed by four hexadecimal digits");
    CHECK(failure("\"open") == "1:6 the text ends inside a string");
    CHECK(failure("[1, 2") == "1:6 the text ends inside an array");
    CHECK(failure("{\"a\": 1") == "1:8 the text ends inside an object");
    CHECK(failure("\"\xC0\x80\"") == "1:2 the text isn't valid UTF-8");
    CHECK(failure("\"\xED\xA0\x80\"") == "1:2 the text isn't valid UTF-8");
    CHECK(failure("{} {}") == "1:4 there is more after the value");
    CHECK_FALSE(json::tryParse("[").has_value());
    CHECK(json::tryParse("[]").has_value());
}

TEST_CASE("JSON nested too deep is refused before the stack runs out")
{
    const auto nested = [](std::size_t depth) { return std::string(depth, '[') + std::string(depth, ']'); };
    CHECK_NOTHROW(json::parse(nested(json::maximumDepth)));
    CHECK_THROWS_AS(json::parse(nested(json::maximumDepth + 1)), json::ParseError);
    CHECK_THROWS_AS(json::parse(std::string(1000000, '[')), json::ParseError);
}

TEST_CASE("JSON writes compact and pretty layouts")
{
    const Value value(Object{{"name", "Take 1"},
                             {"rate", 48000},
                             {"gain", 0.5},
                             {"channels", Array{true, false}},
                             {"empty", Array{}},
                             {"none", Object{}},
                             {"missing", nullptr}});

    CHECK(json::write(value) ==
          R"({"name":"Take 1","rate":48000,"gain":0.5,"channels":[true,false],"empty":[],"none":{},"missing":null})");

    CHECK(json::write(value, json::Layout::pretty) == R"({
  "name": "Take 1",
  "rate": 48000,
  "gain": 0.5,
  "channels": [
    true,
    false
  ],
  "empty": [],
  "none": {},
  "missing": null
})");
}

TEST_CASE("JSON writes escapes, and broken UTF-8 as U+FFFD")
{
    CHECK(json::write("quote \" backslash \\ newline \n tab \t") == R"("quote \" backslash \\ newline \n tab \t")");
    CHECK(json::write(std::string("nul \0 unit \x1f", 12)) == R"("nul \u0000 unit \u001f")");
    CHECK(json::write("/ stays") == R"("/ stays")");
    CHECK(json::write("caf\xC3\xA9") == "\"caf\xC3\xA9\"");
    CHECK(json::write("bad \xC3 byte") == "\"bad \xEF\xBF\xBD byte\"");
    CHECK(json::write(Object{{"\xFF", 1}}) == "{\"\xEF\xBF\xBD\":1}");
}

TEST_CASE("JSON writes numbers as browsers do")
{
    CHECK(json::write(0.1) == "0.1");
    CHECK(json::write(0.1 + 0.2) == "0.30000000000000004");
    CHECK(json::write(48000.0) == "48000");
    CHECK(json::write(1e21) == "1e+21");
    CHECK(json::write(-1.5e-7) == "-1.5e-7");
    CHECK(json::write(-0.0) == "0");
    CHECK(json::write(std::numeric_limits<double>::infinity()) == "null");
    CHECK(json::write(std::nan("")) == "null");
    CHECK(json::write(std::numeric_limits<std::int64_t>::min()) == "-9223372036854775808");
    CHECK(json::write(9007199254740991.0) == "9007199254740991");
}

TEST_CASE("JSON writes a whole real past 2^53 with an exponent, so it reads back as the same real")
{
    const double twoTo62 = 4611686018427387904.0;
    CHECK(json::write(twoTo62) == "4.611686018427388e+18");
    CHECK(json::write(-twoTo62) == "-4.611686018427388e+18");
    CHECK(json::write(1e20) == "1e+20");
    CHECK(json::write(9007199254740992.0) == "9.007199254740992e+15");

    const auto back = json::parse(json::write(twoTo62));
    CHECK(back.isReal());
    CHECK(back.asNumber() == twoTo62);
}

TEST_CASE("JSON reads back what it writes")
{
    std::mt19937_64 random(20261023);
    std::uniform_int_distribution<int> kind(0, 6);

    // Random values nested a few deep, with doubles drawn from every bit pattern.
    const auto make = [&](auto& self, int depth) -> Value {
        switch (depth > 3 ? kind(random) % 5 : kind(random))
        {
            case 0: return nullptr;
            case 1: return random() % 2 == 0;
            case 2: return static_cast<std::int64_t>(random());
            case 3:
            {
                double real = 0;
                do
                {
                    const auto bits = random();
                    std::memcpy(&real, &bits, sizeof real);
                } while (!std::isfinite(real));
                return real;
            }
            case 4:
            {
                std::string text;
                for (auto n = random() % 12; n > 0; --n)
                    text += static_cast<char>(random() % 0x7F + 1);
                return text + "\xE2\x82\xAC";
            }
            case 5:
            {
                Array array;
                for (auto n = random() % 5; n > 0; --n)
                    array.push_back(self(self, depth + 1));
                return array;
            }
            default:
            {
                Object object;
                for (auto n = random() % 5; n > 0; --n)
                    object.set("k" + std::to_string(random() % 100), self(self, depth + 1));
                return object;
            }
        }
    };

    for (int i = 0; i < 2000; ++i)
    {
        const auto value = make(make, 0);
        const auto compact = json::write(value);
        CHECK(json::parse(compact) == value);
        CHECK(json::write(json::parse(compact)) == compact);
        CHECK(json::parse(json::write(value, json::Layout::pretty)) == value);
    }
}
