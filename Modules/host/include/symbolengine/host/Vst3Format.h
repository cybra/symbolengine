#pragma once

#include "PluginFormat.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace symbolengine
{

/** VST3 plugins, hosted as the architecture's hosting section says. A plugin's identity is its class ID's 16 bytes as
    32 hexadecimal digits, its location the bundle, or on Windows the single file, it lies in.

    A module's code, once loaded, stays loaded for as long as the program runs: its entry function is called once,
    when it is first loaded, and its exit function once, when the program ends, after every plugin made from it is
    gone, and it is never unloaded. The format is made and used on the main thread. */
class Vst3Format : public PluginFormat
{
public:
    struct Options
    {
        /** The program's name, as plugins are told it. */
        std::string hostName = "SymbolEngine";

        /** The main thread, as for PluginHost: messages between a plugin's two parts reach them on it, whichever
            thread sent them. */
        std::function<void(std::function<void()>)> post;
        std::function<bool()> onMainThread;
    };

    explicit Vst3Format(Options options);
    ~Vst3Format() override;

    Vst3Format(const Vst3Format&) = delete;
    Vst3Format& operator=(const Vst3Format&) = delete;

    /** The description of the plugin of this class ID in this module, as the host is to be given it. */
    static PluginDescription describe(const std::filesystem::path& module, const std::string& classId,
                                      std::string name = {});

    std::string name() const override;

    /** The module's effect classes, in its factory's order: each class's name, maker, version, SDK and kind, an
        instrument where its subcategories say so. Its code is loaded to read them, and stays loaded. */
    std::vector<PluginDescription> pluginsIn(const std::filesystem::path& file, PluginError& error) override;

    void make(const PluginDescription& plugin, PluginFormatClient& client,
              std::function<void(std::unique_ptr<PluginFormatInstance>, PluginError)> done) override;

    struct Shared;

private:
    std::shared_ptr<Shared> shared;
};

}
