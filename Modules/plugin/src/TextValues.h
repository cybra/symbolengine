#pragma once

#include <symbolengine/plugin/Plugin.h>

#include <optional>
#include <string>
#include <string_view>

namespace symbolengine::plugin::detail
{

/** The number at the start of text typed as a value, such as "-6", "+3.5", ".5" or "-6 dB", with spaces around it
    allowed; "-inf" and "inf" as infinities. Nothing if it doesn't start with a number. */
std::optional<double> leadingNumber(std::string_view text);

/** A parameter's value as text: as the parameter shows it, or the number to two decimal places. */
std::string textFor(const Parameter& parameter, double value);

}
