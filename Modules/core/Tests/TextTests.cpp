#include <symbolengine/core/Text.h>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace symbolengine;

namespace
{
const std::string fffd = "\xEF\xBF\xBD";
}

TEST_CASE("Well-formed UTF-8 is told from broken")
{
    CHECK(text::isValidUtf8(""));
    CHECK(text::isValidUtf8("plain"));
    CHECK(text::isValidUtf8("caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x8E\xA7"));
    CHECK(text::isValidUtf8("\xF4\x8F\xBF\xBF"));   // U+10FFFF

    CHECK_FALSE(text::isValidUtf8("\xC3"));               // cut short
    CHECK_FALSE(text::isValidUtf8("\xC0\x80"));           // overlong
    CHECK_FALSE(text::isValidUtf8("\xE0\x80\x80"));       // overlong
    CHECK_FALSE(text::isValidUtf8("\xED\xA0\x80"));       // a surrogate
    CHECK_FALSE(text::isValidUtf8("\xF4\x90\x80\x80"));   // past U+10FFFF
    CHECK_FALSE(text::isValidUtf8("\x80"));               // a continuation on its own
    CHECK_FALSE(text::isValidUtf8("\xFF"));
}

TEST_CASE("Broken UTF-8 is replaced as browsers replace it")
{
    CHECK(text::replaceInvalidUtf8("fine") == "fine");
    CHECK(text::replaceInvalidUtf8("\xC3") == fffd);
    CHECK(text::replaceInvalidUtf8(std::string("\xF0\x9F\x98") + "A") == fffd + "A");
    CHECK(text::replaceInvalidUtf8("\xE2\x82x") == fffd + "x");
    CHECK(text::replaceInvalidUtf8("\xC0\x80") == fffd + fffd);
    CHECK(text::replaceInvalidUtf8("\xED\xA0\x80") == fffd + fffd + fffd);
    CHECK(text::replaceInvalidUtf8("\xF4\x90\x80\x80") == fffd + fffd + fffd + fffd);
    CHECK(text::replaceInvalidUtf8(std::string("a\xFF") + "b\xC3\xA9") == "a" + fffd + "b\xC3\xA9");
}

TEST_CASE("Trim takes spaces and line breaks off the ends only")
{
    CHECK(text::trim("  a b \t\r\n") == "a b");
    CHECK(text::trim("") == "");
    CHECK(text::trim(" \n ") == "");
    CHECK(text::trim("x") == "x");
}

TEST_CASE("Split keeps empty pieces, so joining gives the text back")
{
    using Pieces = std::vector<std::string_view>;
    CHECK(text::split("a,,b", ",") == Pieces{"a", "", "b"});
    CHECK(text::split("", ",") == Pieces{""});
    CHECK(text::split("a--b--", "--") == Pieces{"a", "b", ""});
    CHECK(text::split("abc", "") == Pieces{"abc"});

    for (const char* original : {"a,,b", ",", "", "x,y,z,"})
        CHECK(text::join(text::split(original, ","), ",") == original);

    CHECK(text::join(std::vector<std::string>{"one", "two"}, " and ") == "one and two");
}

TEST_CASE("Replace all works from left to right")
{
    CHECK(text::replaceAll("aaa", "aa", "b") == "ba");
    CHECK(text::replaceAll("a.b.c", ".", "::") == "a::b::c");
    CHECK(text::replaceAll("x", "", "y") == "x");
    CHECK(text::replaceAll("", "a", "b") == "");
}

TEST_CASE("Case changes only A to Z")
{
    CHECK(text::toLowerAscii("Caf\xC3\x89 ABC") == "caf\xC3\x89 abc");
    CHECK(text::toUpperAscii("caf\xC3\xA9 abc") == "CAF\xC3\xA9 ABC");
    CHECK(text::equalsIgnoringCaseAscii("Content-Type", "content-TYPE"));
    CHECK_FALSE(text::equalsIgnoringCaseAscii("\xC3\xA9", "\xC3\x89"));
    CHECK_FALSE(text::equalsIgnoringCaseAscii("ab", "abc"));
}
