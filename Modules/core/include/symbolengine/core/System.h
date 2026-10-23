#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace symbolengine::system
{

/** An environment variable's value as UTF-8, or nothing if it isn't set. */
std::optional<std::string> environmentVariable(std::string_view name);

}
