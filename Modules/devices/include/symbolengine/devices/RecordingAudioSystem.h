#pragma once

#include "AudioSystem.h"

#include <functional>
#include <memory>
#include <string>

namespace symbolengine
{

/** A system that passes every call to another, the computer's own say, and writes down what happens, a line of JSON
    for each thing: the devices with their descriptions and the defaults each time they change, each notice the system
    sends, and what each stream's device does, its opening, start and stop, its losses with their reasons, its changes
    and requests to be opened again, each gap in its positions and each stretch it gave no blocks. Each line carries
    the seconds since recording began. FakeAudioSystem::replay plays such lines back, so a test meets what real
    hardware did, as a Bluetooth headset switching profiles, an interface unplugged during a take or the computer
    sleeping. Lines are written from the recorder's own threads, never the audio thread. */
class RecordingAudioSystem final : public AudioSystem
{
public:
    RecordingAudioSystem(std::shared_ptr<AudioSystem> real, std::function<void(const std::string& line)> write);
    ~RecordingAudioSystem() override;

    RecordingAudioSystem(const RecordingAudioSystem&) = delete;
    RecordingAudioSystem& operator=(const RecordingAudioSystem&) = delete;

    std::string name() const override;
    std::int64_t now() const noexcept override;
    std::vector<std::string> deviceIds() override;
    AudioDeviceInfo describe(const std::string& id) override;
    std::string defaultDevice(AudioDirection direction) override;
    bool inputMuted() override;
    std::unique_ptr<AudioSystemStream> open(const std::string& id, const AudioStreamRequest& request,
                                            AudioError& error) override;
    void addListener(AudioSystemListener& listener) override;
    void removeListener(AudioSystemListener& listener) override;

    struct Impl;

private:
    std::shared_ptr<Impl> impl;
};

}
