#pragma once

#include <symbolengine/core/Value.h>
#include <symbolengine/devices/AudioSystem.h>

#include <string>

// The lines RecordingAudioSystem writes and FakeAudioSystem::replay reads.
namespace symbolengine::devices::detail
{

Value toValue(const AudioDeviceInfo& device);
AudioDeviceInfo deviceFrom(const Value& value);

const char* nameOf(AudioSystemChange change) noexcept;
const char* nameOf(AudioLoss loss) noexcept;
bool changeNamed(const std::string& name, AudioSystemChange& change) noexcept;
bool lossNamed(const std::string& name, AudioLoss& loss) noexcept;

}
