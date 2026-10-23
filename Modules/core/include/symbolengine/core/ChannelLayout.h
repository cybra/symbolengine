#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace symbolengine
{

/** The speaker a channel is for. Each has the number CLAP gives it, the first eighteen also WAVE files' channel masks,
    and the wide pair, which CLAP has no number for, comes after them. A layout's channels come in this order in WAVE
    files and VST3 hosts. */
enum class Speaker : std::uint8_t
{
    left = 0,
    right = 1,
    center = 2,
    lfe = 3,
    backLeft = 4,   // the surrounds of 5.1, and the rear pair of 7.1
    backRight = 5,
    leftCenter = 6,
    rightCenter = 7,
    backCenter = 8,
    sideLeft = 9,   // the side pair of 7.1
    sideRight = 10,
    top = 11,
    topFrontLeft = 12,
    topFrontCenter = 13,
    topFrontRight = 14,
    topBackLeft = 15,
    topBackCenter = 16,
    topBackRight = 17,
    topSideLeft = 18,   // the top pair of 5.1.2 and 7.1.2
    topSideRight = 19,
    wideLeft = 20,   // the wide pair of Dolby's 9.1.4 and 9.1.6
    wideRight = 21,
};

/** How many kinds of speaker there are, each numbered below this. */
inline constexpr std::size_t speakerKinds = 22;

/** A bus's or a file's channels: the speaker each one is for, in order. */
class ChannelLayout
{
public:
    /** No channels. */
    ChannelLayout() = default;
    ChannelLayout(std::initializer_list<Speaker> speakers);
    explicit ChannelLayout(std::vector<Speaker> speakers);

    // The standard layouts, each in the order of the speakers' numbers.
    static ChannelLayout mono();             // center
    static ChannelLayout stereo();           // left, right
    static ChannelLayout lcr();              // left, right, center
    static ChannelLayout quad();             // left, right, back left, back right
    static ChannelLayout surround50();       // L R C Ls Rs
    static ChannelLayout surround51();       // L R C LFE Ls Rs
    static ChannelLayout surround70();       // L R C Lrs Rrs Lss Rss
    static ChannelLayout surround71();       // L R C LFE Lrs Rrs Lss Rss
    static ChannelLayout surround512();      // 5.1 with top side left and right
    static ChannelLayout surround514();      // 5.1 with top front and top back pairs
    static ChannelLayout surround712();      // 7.1 with top side left and right
    static ChannelLayout surround714();      // 7.1 with top front and top back pairs
    static ChannelLayout surround914Itu();   // 7.1.4 with left and right center, where Dolby's 9.1.4 has wides
    static ChannelLayout surround916Itu();   // 9.1.4 ITU with top side left and right
    static ChannelLayout surround914();      // Dolby's: 7.1.4 with wide left and right
    static ChannelLayout surround916();      // Dolby's: 9.1.4 with top side left and right

    /** Every standard layout, from mono to 9.1.6, in the order above. */
    static std::span<const ChannelLayout> standard();

    /** The layout of a channel mask, (1 << speaker) for each speaker, in the order of the speakers' numbers; nothing
        for a bit with no speaker. */
    static std::optional<ChannelLayout> fromMask(std::uint64_t mask);

    std::uint32_t channels() const noexcept { return static_cast<std::uint32_t>(list.size()); }
    std::span<const Speaker> speakers() const noexcept { return list; }

    /** The channel a speaker is on, or nothing if the layout doesn't have it. */
    std::optional<std::size_t> channelOf(Speaker speaker) const noexcept;

    /** Whether no speaker is on two channels. */
    bool valid() const noexcept;

    /** The standard name, such as "Mono", "5.1", "7.1.4" or "9.1.6 ITU", or the number of channels, such as
        "3 channels". */
    std::string name() const;

    bool operator==(const ChannelLayout& other) const = default;

private:
    std::vector<Speaker> list;
};

}
