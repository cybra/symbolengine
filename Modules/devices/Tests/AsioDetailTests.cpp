// What the ASIO layer works out without a driver, on every computer: each sample type read and written exactly, the
// block sizes a driver's answer offers, and its strings bounded.

#include "AsioDetail.h"

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

using namespace symbolengine::asio::detail;

namespace
{
// The bytes a driver gives for a whole number, written out apart from the code under test.
std::vector<unsigned char> bytesOf(std::int64_t value, int bytes, bool bigEndian)
{
    std::vector<unsigned char> out(static_cast<std::size_t>(bytes));
    const auto raw = static_cast<std::uint64_t>(value);
    for (int i = 0; i < bytes; ++i)
    {
        const auto byte = static_cast<unsigned char>((raw >> (8 * i)) & 0xFF);
        out[static_cast<std::size_t>(bigEndian ? bytes - 1 - i : i)] = byte;
    }
    return out;
}

const std::vector<SampleLayout> wholeNumbers{
    {2, 16, false, false}, {2, 16, false, true}, {3, 24, false, false}, {3, 24, false, true},
    {4, 32, false, false}, {4, 32, false, true}, {4, 16, false, false}, {4, 16, false, true},
    {4, 18, false, false}, {4, 18, false, true}, {4, 20, false, false}, {4, 20, false, true},
    {4, 24, false, false}, {4, 24, false, true}};
}

TEST_CASE("Every whole-number type reads its smallest, largest and zero values and a step either side exactly")
{
    for (const auto& layout : wholeNumbers)
    {
        const auto top = std::int64_t{1} << (layout.bits - 1);
        for (const auto value : {-top, -top + 1, std::int64_t{-1}, std::int64_t{0}, std::int64_t{1}, top - 2, top - 1})
        {
            INFO(layout.bytes << " bytes, " << layout.bits << " bits, " << (layout.bigEndian ? "big" : "little")
                              << " endian, value " << value);
            const auto given = bytesOf(value, layout.bytes, layout.bigEndian);
            float got = 99.0f;
            read(layout, given.data(), &got, 1);
            CHECK(got == static_cast<float>(static_cast<double>(value) / static_cast<double>(top)));

            // Back the other way: a value a float holds exactly comes back as the same bytes.
            if (layout.bits <= 24)
            {
                std::vector<unsigned char> written(static_cast<std::size_t>(layout.bytes), 0xAA);
                write(layout, &got, written.data(), 1);
                CHECK(written == given);
            }
        }
    }
}

TEST_CASE("A container holding fewer bits takes its sign from its top valid bit, whatever lies above it")
{
    // An 18-bit value of -1 in a 32-bit container, its upper bits cleared, as some drivers leave them.
    const SampleLayout layout{4, 18, false, false};
    const std::vector<unsigned char> given{0xFF, 0xFF, 0x03, 0x00};
    float got = 0.0f;
    read(layout, given.data(), &got, 1);
    CHECK(got == static_cast<float>(-1.0 / 131072.0));
}

TEST_CASE("Writing holds values to the type's range, rounds half steps to even, and writes 0 for what isn't a number")
{
    for (const auto& layout : wholeNumbers)
    {
        INFO(layout.bytes << " bytes, " << layout.bits << " bits, " << (layout.bigEndian ? "big" : "little") << " endian");
        const auto top = std::int64_t{1} << (layout.bits - 1);
        const auto step = std::ldexp(1.0f, 1 - layout.bits);
        const std::vector<std::pair<float, std::int64_t>> cases{
            {1.0f, top - 1},
            {-1.0f, -top},
            {2.0f, top - 1},
            {-2.0f, -top},
            {std::numeric_limits<float>::infinity(), top - 1},
            {std::numeric_limits<float>::quiet_NaN(), 0},
            {0.5f * step, 0},
            {1.5f * step, 2},
            {-1.5f * step, -2}};
        for (const auto& [value, expected] : cases)
        {
            INFO("value " << value);
            std::vector<unsigned char> written(static_cast<std::size_t>(layout.bytes), 0xAA);
            write(layout, &value, written.data(), 1);
            CHECK(written == bytesOf(expected, layout.bytes, layout.bigEndian));
        }
    }
}

TEST_CASE("Floating-point types read and write in both byte orders, doubles taken to the nearest float")
{
    for (const bool big : {false, true})
    {
        INFO((big ? "big" : "little") << " endian");
        for (const float value : {0.25f, -1.0f, 1e-30f, 3.0f})
        {
            const auto single = bytesOf(static_cast<std::int64_t>(std::bit_cast<std::uint32_t>(value)), 4, big);
            float got = 0.0f;
            read({4, 32, true, big}, single.data(), &got, 1);
            CHECK(got == value);
            std::vector<unsigned char> written(4, 0xAA);
            write({4, 32, true, big}, &value, written.data(), 1);
            CHECK(written == single);

            const auto wide = bytesOf(static_cast<std::int64_t>(std::bit_cast<std::uint64_t>(static_cast<double>(value))), 8, big);
            got = 0.0f;
            read({8, 64, true, big}, wide.data(), &got, 1);
            CHECK(got == value);
            std::vector<unsigned char> writtenWide(8, 0xAA);
            write({8, 64, true, big}, &value, writtenWide.data(), 1);
            CHECK(writtenWide == wide);
        }
    }

    // A double between two floats reads as the nearer.
    const auto between = bytesOf(static_cast<std::int64_t>(std::bit_cast<std::uint64_t>(0.1)), 8, false);
    float got = 0.0f;
    read({8, 64, true, false}, between.data(), &got, 1);
    CHECK(got == 0.1f);
}

TEST_CASE("Frames are read and written one container apart, and a type that isn't converted reads as silence")
{
    const SampleLayout layout{3, 24, false, false};
    const std::vector<float> ramp{0.0f, 0.5f, -0.5f, 0.25f};
    std::vector<unsigned char> buffer(12, 0);
    write(layout, ramp.data(), buffer.data(), ramp.size());
    std::vector<float> back(4, 9.0f);
    read(layout, buffer.data(), back.data(), back.size());
    CHECK(back == ramp);

    std::vector<float> silent(4, 9.0f);
    read(SampleLayout{}, buffer.data(), silent.data(), silent.size());
    CHECK(silent == std::vector<float>(4, 0.0f));
}

TEST_CASE("A driver's buffer sizes are offered as it gives them, its preferred size first")
{
    // Steps of 16 from 8, within 64 sizes: all of them, with the largest and the preferred.
    const auto steps = blockSizes(8, 1024, 32, 16);
    REQUIRE(!steps.empty());
    CHECK(steps.front() == 32);
    CHECK(steps[1] == 8);
    CHECK(steps[2] == 24);
    CHECK(steps.back() == 1024);
    CHECK(steps.size() == 66);

    CHECK(blockSizes(64, 2048, 256, -1) == std::vector<int>{256, 64, 128, 512, 1024, 2048});
    CHECK(blockSizes(512, 512, 512, 0) == std::vector<int>{512});
    CHECK(blockSizes(64, 1024, 256, 0) == std::vector<int>{256, 64, 1024});

    // Steps of one through thousands of sizes: those that are powers of two, the smallest and the largest.
    CHECK(blockSizes(32, 4000, 128, 1) == std::vector<int>{128, 32, 64, 256, 512, 1024, 2048, 4000});

    // Answers that can't be right offer the preferred size alone, or nothing.
    CHECK(blockSizes(1, 1L << 30, 256, -1) == std::vector<int>{256});
    CHECK(blockSizes(0, -5, 0, 7).empty());
}

TEST_CASE("A driver's strings are read to their first zero or their bound, and only valid UTF-8 is taken as UTF-8")
{
    std::vector<char> unterminated(48, 'x');
    CHECK(bounded(unterminated.data(), 32) == std::string(32, 'x'));
    CHECK(bounded("Input 1", 32) == "Input 1");
    CHECK(bounded(nullptr, 32).empty());

    CHECK(isUtf8("Mic/Line 1"));
    CHECK(isUtf8("Entr\xC3\xA9" "e 1"));
    CHECK(isUtf8("\xF0\x9F\x8E\xA4"));
    CHECK_FALSE(isUtf8("Entr\xE9" "e 1"));
    CHECK_FALSE(isUtf8("\xC0\xAF"));
    CHECK_FALSE(isUtf8("\xE2\x82"));
    CHECK_FALSE(isUtf8("\xED\xA0\x80"));
}
