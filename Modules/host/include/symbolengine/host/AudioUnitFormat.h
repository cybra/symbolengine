#pragma once

#include "PluginFormat.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace symbolengine
{

#if defined(__APPLE__)

/** Audio Units v2 on a Mac, hosted as the architecture's hosting section says. A plugin's identity is its three codes
    (PluginIdentity::audioUnit), its location the component bundle it lies in, and its kind its type: an effect for
    aufx and aumf, an instrument for aumu, other kinds for the rest.

    Units are made through the system's registry, which knows those installed and those a program registered with
    itself, without blocking the main thread; their completion comes to the main thread, whatever thread the system
    gives it on. Everything but rendering is called on the main thread: each unit's largest block and every bus's
    format set before it is initialized and read back after, stereo asked for whatever it starts with. Renders split a
    block longer than the unit takes, give its input exactly as it pulls it, take its output wherever it put it, silence
    a channel it left unwritten and never trust its flag that the output is silent; a failed render passes the block as
    it came, three in a row or an instance the system says is gone failing the plugin. The format is made and used on
    the main thread. */
class AudioUnitFormat : public PluginFormat
{
public:
    struct Options
    {
        /** The main thread, as for PluginHost: the system's completion of making a unit reaches it there. */
        std::function<void(std::function<void()>)> post;
        std::function<bool()> onMainThread;
    };

    explicit AudioUnitFormat(Options options);
    ~AudioUnitFormat() override;

    AudioUnitFormat(const AudioUnitFormat&) = delete;
    AudioUnitFormat& operator=(const AudioUnitFormat&) = delete;

    std::string name() const override;

    /** /Library/Audio/Plug-Ins/Components and the user's own. */
    std::vector<PluginFolder> folders() const override;

    /** A ".component" bundle. */
    bool isPluginFile(const std::filesystem::path& path) const override;

    /** The stamp of the binary its Info.plist names, its version from its Info.plist, and its binary's architectures. */
    std::optional<PluginFileFacts> examine(const std::filesystem::path& file, PluginError& error) const override;

    /** The units its Info.plist declares, read without loading its code: each one's codes, its name, split at its first
        colon into maker and name, its version and its kind. */
    std::vector<PluginDescription> pluginsIn(const std::filesystem::path& file, PluginError& error) override;

    void make(const PluginDescription& plugin, PluginFormatClient& client,
              std::function<void(std::unique_ptr<PluginFormatInstance>, PluginError)> done) override;

    struct Shared;

private:
    std::shared_ptr<Shared> shared;
};

#endif

}
