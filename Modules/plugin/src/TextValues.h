#pragma once

#include <optional>
#include <string_view>

namespace symbolengine::plugin::detail
{

/** The number at the start of text typed as a value, such as "-6", "+3.5", ".5" or "-6 dB", with spaces around it
    allowed; "-inf" and "inf" as infinities. Nothing if it doesn't start with a number. */
std::optional<double> leadingNumber(std::string_view text);

}
