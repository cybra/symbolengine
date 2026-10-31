// How a channel is read out of a buffer Core Audio gives, and written into one, apart from Core Audio so they run anywhere.

#include "CoreAudioDetail.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>

using namespace symbolengine::coreAudio::detail;

TEST_CASE("A channel of a buffer interleaved as it was opened is read and written in place")
{
    const std::array<float, 8> stereo{1, 10, 2, 20, 3, 30, 4, 40};
    std::array<float, 4> read{};
    CHECK(readChannel(stereo.data(), sizeof stereo, 2, 1, 4, read.data()));
    CHECK(read == std::array<float, 4>{10, 20, 30, 40});

    std::array<float, 8> out{};
    const std::array<float, 4> from{5, 6, 7, 8};
    CHECK(writeChannel(from.data(), 4, out.data(), sizeof out, 2, 1));
    CHECK(out == std::array<float, 8>{0, 5, 0, 6, 0, 7, 0, 8});
}

TEST_CASE("A device going mono under a stream never has a channel it no longer has read or written")
{
    // A channel past what the buffer interleaves now, for the cycles before the stream hears of it.
    std::array<float, 5> mono{9, 9, 9, 9, 9};
    std::array<float, 4> read{7, 7, 7, 7};
    CHECK_FALSE(readChannel(mono.data(), 4 * sizeof(float), 1, 1, 4, read.data()));

    const std::array<float, 4> from{5, 6, 7, 8};
    CHECK_FALSE(writeChannel(from.data(), 4, mono.data(), 4 * sizeof(float), 1, 1));
    CHECK(mono == std::array<float, 5>{9, 9, 9, 9, 9});
}

TEST_CASE("A buffer shorter than the cycle is read as far as it goes, and the rest is silence, and written no further")
{
    const std::array<float, 4> two{1, 10, 2, 20};
    std::array<float, 4> read{7, 7, 7, 7};
    CHECK(readChannel(two.data(), sizeof two, 2, 0, 4, read.data()));
    CHECK(read == std::array<float, 4>{1, 2, 0, 0});

    std::array<float, 5> out{9, 9, 9, 9, 9};
    const std::array<float, 4> from{5, 6, 7, 8};
    CHECK(writeChannel(from.data(), 4, out.data(), 4 * sizeof(float), 2, 0));
    CHECK(out == std::array<float, 5>{5, 9, 6, 9, 9});
}
