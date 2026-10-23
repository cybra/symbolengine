// clap-wrapper's VST3 speaker arrangements as SymbolEngine's fork maps them, on their own: each arrangement's channel
// map in CLAP's numbers, and back. Dolby's wide pair has numbers only for a plugin that takes it.

#include "detail/vst3/channelmap.h"

#include <clapwrapper/wide_speakers.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

using namespace Steinberg;
using Clap::Vst3Channels::arrangementFromChannelMap;
using Clap::Vst3Channels::channelMapFromArrangement;

TEST_CASE("Dolby's 9.1.6 and 9.1.4 have channel maps only for a plugin that takes the wide pair")
{
    const std::vector<std::uint8_t> dolby916{
        CLAP_SURROUND_FL,  CLAP_SURROUND_FR,  CLAP_SURROUND_FC,         CLAP_SURROUND_LFE,
        CLAP_SURROUND_BL,  CLAP_SURROUND_BR,  CLAP_SURROUND_SL,         CLAP_SURROUND_SR,
        CLAP_SURROUND_TFL, CLAP_SURROUND_TFR, CLAP_SURROUND_TBL,        CLAP_SURROUND_TBR,
        CLAP_SURROUND_TSL, CLAP_SURROUND_TSR, CLAP_WRAPPER_SURROUND_LW, CLAP_WRAPPER_SURROUND_RW};
    const std::vector<std::uint8_t> dolby914{
        CLAP_SURROUND_FL,  CLAP_SURROUND_FR,  CLAP_SURROUND_FC,         CLAP_SURROUND_LFE,       CLAP_SURROUND_BL,
        CLAP_SURROUND_BR,  CLAP_SURROUND_SL,  CLAP_SURROUND_SR,         CLAP_SURROUND_TFL,       CLAP_SURROUND_TFR,
        CLAP_SURROUND_TBL, CLAP_SURROUND_TBR, CLAP_WRAPPER_SURROUND_LW, CLAP_WRAPPER_SURROUND_RW};

    std::vector<std::uint8_t> map;
    CHECK_FALSE(channelMapFromArrangement(Vst::SpeakerArr::k91_6_W, map));
    CHECK(map.empty());

    REQUIRE(channelMapFromArrangement(Vst::SpeakerArr::k91_6_W, map, true));
    CHECK(map == dolby916);
    CHECK(arrangementFromChannelMap(dolby916.data(), static_cast<std::uint32_t>(dolby916.size())) ==
          Vst::SpeakerArr::k91_6_W);

    REQUIRE(channelMapFromArrangement(Vst::SpeakerArr::k91_4_W, map, true));
    CHECK(map == dolby914);
    CHECK(arrangementFromChannelMap(dolby914.data(), static_cast<std::uint32_t>(dolby914.size())) ==
          Vst::SpeakerArr::k91_4_W);
}

TEST_CASE("The ITU 9.1.6 and Atmos 7.1.4 need no wide pair")
{
    std::vector<std::uint8_t> map;
    REQUIRE(channelMapFromArrangement(Vst::SpeakerArr::k91_6, map));
    CHECK(map == std::vector<std::uint8_t>{CLAP_SURROUND_FL, CLAP_SURROUND_FR, CLAP_SURROUND_FC, CLAP_SURROUND_LFE,
                                           CLAP_SURROUND_BL, CLAP_SURROUND_BR, CLAP_SURROUND_FLC, CLAP_SURROUND_FRC,
                                           CLAP_SURROUND_SL, CLAP_SURROUND_SR, CLAP_SURROUND_TFL, CLAP_SURROUND_TFR,
                                           CLAP_SURROUND_TBL, CLAP_SURROUND_TBR, CLAP_SURROUND_TSL, CLAP_SURROUND_TSR});

    REQUIRE(channelMapFromArrangement(Vst::SpeakerArr::k71_4, map));
    CHECK(map == std::vector<std::uint8_t>{CLAP_SURROUND_FL, CLAP_SURROUND_FR, CLAP_SURROUND_FC, CLAP_SURROUND_LFE,
                                           CLAP_SURROUND_BL, CLAP_SURROUND_BR, CLAP_SURROUND_SL, CLAP_SURROUND_SR,
                                           CLAP_SURROUND_TFL, CLAP_SURROUND_TFR, CLAP_SURROUND_TBL, CLAP_SURROUND_TBR});
}

TEST_CASE("A map out of VST3's order, or with a speaker twice, has no arrangement")
{
    const std::uint8_t backwards[]{CLAP_SURROUND_FR, CLAP_SURROUND_FL};
    const std::uint8_t twice[]{CLAP_SURROUND_FL, CLAP_SURROUND_FL};
    const std::uint8_t widesFirst[]{CLAP_WRAPPER_SURROUND_LW, CLAP_SURROUND_FL};
    CHECK(arrangementFromChannelMap(backwards, 2) == Vst::SpeakerArr::kEmpty);
    CHECK(arrangementFromChannelMap(twice, 2) == Vst::SpeakerArr::kEmpty);
    CHECK(arrangementFromChannelMap(widesFirst, 2) == Vst::SpeakerArr::kEmpty);
}
