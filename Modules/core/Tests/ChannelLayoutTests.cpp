#include <symbolengine/core/ChannelLayout.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

using namespace symbolengine;

TEST_CASE("The standard layouts have the channels and names their formats give them")
{
    const std::vector<std::pair<ChannelLayout, std::string>> expected{{ChannelLayout::mono(), "Mono"},
                                                                      {ChannelLayout::stereo(), "Stereo"},
                                                                      {ChannelLayout::lcr(), "LCR"},
                                                                      {ChannelLayout::quad(), "Quad"},
                                                                      {ChannelLayout::surround50(), "5.0"},
                                                                      {ChannelLayout::surround51(), "5.1"},
                                                                      {ChannelLayout::surround70(), "7.0"},
                                                                      {ChannelLayout::surround71(), "7.1"},
                                                                      {ChannelLayout::surround512(), "5.1.2"},
                                                                      {ChannelLayout::surround514(), "5.1.4"},
                                                                      {ChannelLayout::surround712(), "7.1.2"},
                                                                      {ChannelLayout::surround714(), "7.1.4"},
                                                                      {ChannelLayout::surround914Itu(), "9.1.4 ITU"},
                                                                      {ChannelLayout::surround916Itu(), "9.1.6 ITU"}};

    REQUIRE(ChannelLayout::standard().size() == expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i)
    {
        const auto& [layout, name] = expected[i];
        CHECK(ChannelLayout::standard()[i] == layout);
        CHECK(layout.name() == name);
        CHECK(layout.valid());

        // Each in the order of its speakers' numbers, as WAVE files and VST3 hosts give the channels.
        CHECK(std::is_sorted(layout.speakers().begin(), layout.speakers().end()));
    }

    // The channel counts: a number in each name adds up to it, and the rest are counted.
    CHECK(ChannelLayout::mono().channels() == 1);
    CHECK(ChannelLayout::quad().channels() == 4);
    CHECK(ChannelLayout::surround51().channels() == 6);
    CHECK(ChannelLayout::surround71().channels() == 8);
    CHECK(ChannelLayout::surround514().channels() == 10);
    CHECK(ChannelLayout::surround714().channels() == 12);
    CHECK(ChannelLayout::surround914Itu().channels() == 14);
    CHECK(ChannelLayout::surround916Itu().channels() == 16);
}

TEST_CASE("A layout finds the channel each speaker is on")
{
    const auto layout = ChannelLayout::surround71();
    CHECK(layout.channelOf(Speaker::left) == 0u);
    CHECK(layout.channelOf(Speaker::lfe) == 3u);
    CHECK(layout.channelOf(Speaker::sideRight) == 7u);
    CHECK_FALSE(layout.channelOf(Speaker::topFrontLeft).has_value());
    CHECK(ChannelLayout::surround714().channelOf(Speaker::topBackRight) == 11u);
}

TEST_CASE("A channel mask gives the layout of its speakers")
{
    const auto bit = [](Speaker speaker) { return std::uint64_t{1} << static_cast<unsigned>(speaker); };

    CHECK(ChannelLayout::fromMask(bit(Speaker::left) | bit(Speaker::right)) == ChannelLayout::stereo());
    CHECK(ChannelLayout::fromMask(0x3F) == ChannelLayout::surround51());
    CHECK(ChannelLayout::fromMask(0) == ChannelLayout());
    CHECK_FALSE(ChannelLayout::fromMask(std::uint64_t{1} << 20).has_value());
}

TEST_CASE("Any other layout is named by its channels, and one with a speaker twice is invalid")
{
    CHECK(ChannelLayout({Speaker::left}).name() == "1 channel");
    CHECK(ChannelLayout({Speaker::right, Speaker::left}).name() == "2 channels");
    CHECK(ChannelLayout().name() == "0 channels");
    CHECK(ChannelLayout({Speaker::right, Speaker::left}) != ChannelLayout::stereo());

    CHECK_FALSE(ChannelLayout({Speaker::left, Speaker::left}).valid());
    CHECK_FALSE(ChannelLayout({static_cast<Speaker>(20)}).valid());
}
