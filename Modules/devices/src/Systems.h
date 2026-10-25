#pragma once

#include <symbolengine/devices/AudioSystem.h>

#include <memory>
#include <vector>

namespace symbolengine::devices::detail
{

/** This computer's own audio systems, those built in: Core Audio on a Mac, Windows Audio on Windows. */
std::vector<std::shared_ptr<AudioSystem>> systemsOfThisComputer();

}
