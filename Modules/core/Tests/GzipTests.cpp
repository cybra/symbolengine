#include "symbolengine/core/Gzip.h"

#include <catch2/catch_test_macros.hpp>

#include <random>
#include <string>

using namespace symbolengine;

TEST_CASE("Compressed bytes come back as they were")
{
    std::minstd_rand random(9);
    std::string noise(100000, '\0');

    for (auto& byte : noise)
        byte = static_cast<char>(random());

    for (const std::string& bytes : {std::string(), std::string("a"), std::string(200000, 'x'), noise})
    {
        const auto back = gzip::decompress(gzip::compress(bytes));
        REQUIRE(back.has_value());
        CHECK(*back == bytes);
    }
}

TEST_CASE("Repeated bytes come out smaller, in a file gzip's own header begins")
{
    const auto file = gzip::compress(std::string(200000, 'x'));
    CHECK(file.size() < 1000);
    REQUIRE(file.size() >= 18);
    CHECK(static_cast<unsigned char>(file[0]) == 0x1f);
    CHECK(static_cast<unsigned char>(file[1]) == 0x8b);
    CHECK(file[2] == 8);
    CHECK(file[3] == 0);
}

TEST_CASE("A gzip file made elsewhere is read, its header's name passed over")
{
    // "hello\n" through gzip -9 with the file's name kept.
    const unsigned char made[] = {0x1f, 0x8b, 0x08, 0x08, 0xde, 0xad, 0xbe, 0xef, 0x02, 0x03, 'h',
                                  'i',  '.',  't',  'x',  't',  0x00, 0xcb, 0x48, 0xcd, 0xc9, 0xc9,
                                  0xe7, 0x02, 0x00, 0x20, 0x30, 0x3a, 0x36, 0x06, 0x00, 0x00, 0x00};
    const auto back = gzip::decompress(std::string_view(reinterpret_cast<const char*>(made), sizeof made));
    REQUIRE(back.has_value());
    CHECK(*back == "hello\n");
}

TEST_CASE("A file changed or cut short isn't read as gzip")
{
    const auto file = gzip::compress("the same words again, the same words again, the same words again");
    CHECK_FALSE(gzip::decompress("").has_value());
    CHECK_FALSE(gzip::decompress("not gzip at all, though long enough").has_value());

    for (const auto at : {std::size_t(12), file.size() - 8, file.size() - 1})
    {
        auto changed = file;
        changed[at] = static_cast<char>(changed[at] ^ 0x40);
        CHECK_FALSE(gzip::decompress(changed).has_value());
    }

    CHECK_FALSE(gzip::decompress(file.substr(0, file.size() - 1)).has_value());
    CHECK_FALSE(gzip::decompress(file.substr(0, file.size() / 2)).has_value());
}
