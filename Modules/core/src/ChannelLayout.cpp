#include <symbolengine/core/ChannelLayout.h>

#include <algorithm>
#include <array>
#include <bitset>
#include <utility>

namespace symbolengine
{

namespace
{
using enum Speaker;

struct Named
{
    const char* name;
    ChannelLayout layout;
};

const std::array<Named, 14>& named()
{
    static const std::array<Named, 14> layouts{{
        {"Mono", {center}},
        {"Stereo", {left, right}},
        {"LCR", {left, right, center}},
        {"Quad", {left, right, backLeft, backRight}},
        {"5.0", {left, right, center, backLeft, backRight}},
        {"5.1", {left, right, center, lfe, backLeft, backRight}},
        {"7.0", {left, right, center, backLeft, backRight, sideLeft, sideRight}},
        {"7.1", {left, right, center, lfe, backLeft, backRight, sideLeft, sideRight}},
        {"5.1.2", {left, right, center, lfe, backLeft, backRight, topSideLeft, topSideRight}},
        {"5.1.4",
         {left, right, center, lfe, backLeft, backRight, topFrontLeft, topFrontRight, topBackLeft, topBackRight}},
        {"7.1.2", {left, right, center, lfe, backLeft, backRight, sideLeft, sideRight, topSideLeft, topSideRight}},
        {"7.1.4",
         {left, right, center, lfe, backLeft, backRight, sideLeft, sideRight, topFrontLeft, topFrontRight, topBackLeft,
          topBackRight}},
        {"9.1.4 ITU",
         {left, right, center, lfe, backLeft, backRight, leftCenter, rightCenter, sideLeft, sideRight, topFrontLeft,
          topFrontRight, topBackLeft, topBackRight}},
        {"9.1.6 ITU",
         {left, right, center, lfe, backLeft, backRight, leftCenter, rightCenter, sideLeft, sideRight, topFrontLeft,
          topFrontRight, topBackLeft, topBackRight, topSideLeft, topSideRight}},
    }};
    return layouts;
}

const ChannelLayout& at(std::size_t index)
{
    return named()[index].layout;
}
}

ChannelLayout::ChannelLayout(std::initializer_list<Speaker> speakers) : list(speakers) {}

ChannelLayout::ChannelLayout(std::vector<Speaker> speakers) : list(std::move(speakers)) {}

ChannelLayout ChannelLayout::mono()
{
    return at(0);
}

ChannelLayout ChannelLayout::stereo()
{
    return at(1);
}

ChannelLayout ChannelLayout::lcr()
{
    return at(2);
}

ChannelLayout ChannelLayout::quad()
{
    return at(3);
}

ChannelLayout ChannelLayout::surround50()
{
    return at(4);
}

ChannelLayout ChannelLayout::surround51()
{
    return at(5);
}

ChannelLayout ChannelLayout::surround70()
{
    return at(6);
}

ChannelLayout ChannelLayout::surround71()
{
    return at(7);
}

ChannelLayout ChannelLayout::surround512()
{
    return at(8);
}

ChannelLayout ChannelLayout::surround514()
{
    return at(9);
}

ChannelLayout ChannelLayout::surround712()
{
    return at(10);
}

ChannelLayout ChannelLayout::surround714()
{
    return at(11);
}

ChannelLayout ChannelLayout::surround914Itu()
{
    return at(12);
}

ChannelLayout ChannelLayout::surround916Itu()
{
    return at(13);
}

std::span<const ChannelLayout> ChannelLayout::standard()
{
    static const auto layouts = [] {
        std::vector<ChannelLayout> list;
        for (const auto& entry : named())
            list.push_back(entry.layout);
        return list;
    }();
    return layouts;
}

std::optional<ChannelLayout> ChannelLayout::fromMask(std::uint64_t mask)
{
    if ((mask >> speakerKinds) != 0)
        return std::nullopt;
    std::vector<Speaker> speakers;
    for (std::size_t bit = 0; bit < speakerKinds; ++bit)
        if ((mask >> bit) & 1)
            speakers.push_back(static_cast<Speaker>(bit));
    return ChannelLayout(std::move(speakers));
}

std::optional<std::size_t> ChannelLayout::channelOf(Speaker speaker) const noexcept
{
    const auto found = std::find(list.begin(), list.end(), speaker);
    if (found == list.end())
        return std::nullopt;
    return static_cast<std::size_t>(found - list.begin());
}

bool ChannelLayout::valid() const noexcept
{
    std::bitset<speakerKinds> seen;
    for (const auto speaker : list)
    {
        const auto number = static_cast<std::size_t>(speaker);
        if (number >= speakerKinds || seen[number])
            return false;
        seen[number] = true;
    }
    return true;
}

std::string ChannelLayout::name() const
{
    for (const auto& entry : named())
        if (entry.layout == *this)
            return entry.name;
    return std::to_string(list.size()) + (list.size() == 1 ? " channel" : " channels");
}

}
