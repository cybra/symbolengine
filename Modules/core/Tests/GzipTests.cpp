#include "symbolengine/core/Gzip.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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

namespace
{
std::string header()
{
    return std::string("\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\xff", 10);
}
}

TEST_CASE("Data that isn't deflate's is refused, whatever its last eight bytes say")
{
    // A block of a type that doesn't exist, followed by the checksum and length of nothing, as a tail a crash zeroed
    // leaves it.
    CHECK_FALSE(gzip::decompress(header() + std::string("\xff\xff\xff\xff", 4) + std::string(8, '\0')).has_value());

    // Nothing where the data should be.
    CHECK_FALSE(gzip::decompress(header() + std::string(8, '\0')).has_value());

    // The real thing with its tail zeroed, and cut short.
    auto file = gzip::compress("some bytes worth keeping");
    auto zeroed = file;
    std::fill(zeroed.end() - 8, zeroed.end(), '\0');
    CHECK_FALSE(gzip::decompress(zeroed).has_value());
    const auto cut = header() + file.substr(10, 4) + file.substr(file.size() - 8);
    CHECK_FALSE(gzip::decompress(cut).has_value());
}

TEST_CASE("Bytes between the data's end and the checksum are refused")
{
    auto file = gzip::compress("some bytes worth keeping");
    file.insert(file.size() - 8, "extra");
    CHECK_FALSE(gzip::decompress(file).has_value());
}

TEST_CASE("Data unpacks no further than a limit, or than the length it states")
{
    const std::string bytes(1000000, 'x');
    const auto file = gzip::compress(bytes);

    CHECK(gzip::decompress(file, bytes.size()) == bytes);
    CHECK_FALSE(gzip::decompress(file, bytes.size() - 1).has_value());
    CHECK_FALSE(gzip::decompress(file, 1000).has_value());

    // Stating less than it holds.
    auto understated = file;
    understated[understated.size() - 3] = '\0';
    understated[understated.size() - 2] = '\0';
    understated[understated.size() - 1] = '\0';
    CHECK_FALSE(gzip::decompress(understated).has_value());
}
