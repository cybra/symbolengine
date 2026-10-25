#pragma once

#include <symbolengine/devices/AudioWorkgroup.h>

// How each system's streams make the workgroup a program's threads join.
namespace symbolengine::devices::detail
{

/** On a Mac, from the os_workgroup_t a device gives, already retained, which the workgroup releases. */
AudioWorkgroup appleWorkgroup(void* retained);

/** On Windows, the multimedia scheduler's Pro Audio class. */
AudioWorkgroup proAudioWorkgroup();

}
