#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace symbolengine::plugin::detail
{

/** A VST3 class ID written as Steinberg's validator prints it, 32 hexadecimal digits with any dashes, braces or spaces
    ignored, as the 16 bytes clap-wrapper takes: in the order Windows lays out a GUID, which clap-wrapper turns into
    each other system's order. Nothing if it isn't 32 hexadecimal digits. */
std::optional<std::array<std::uint8_t, 16>> vst3ComponentId(std::string_view written);

}
