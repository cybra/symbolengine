#pragma once

#include "symbolengine/host/PluginFormat.h"

#include "pluginterfaces/base/ipluginbase.h"

#include <atomic>
#include <filesystem>
#include <memory>

namespace symbolengine
{

/** A VST3 module's code: loaded and entered once in the process, and never unloaded. Its exit function is called once,
    when the program ends, after every plugin made from it is gone, before the code's own static objects are taken
    down; a module with a plugin still alive then is left as it is. */
class Vst3Module
{
public:
    /** The module at this location, loaded and entered if it wasn't, or nothing with the error set. */
    static std::shared_ptr<Vst3Module> open(const std::filesystem::path& location, PluginError& error);

    Vst3Module(const Vst3Module&) = delete;
    Vst3Module& operator=(const Vst3Module&) = delete;

    Steinberg::IPluginFactory* factory() const { return pluginFactory; }

    /** How many of its plugins are alive, kept by the format as it makes and takes them apart. */
    std::atomic<int> plugins{0};

    struct Native;

private:
    Vst3Module() = default;

    /** Each system's loading: the binary for this system found in the bundle, loaded, entered, its factory taken. */
    static std::shared_ptr<Native> load(const std::filesystem::path& location, Steinberg::IPluginFactory*& factory,
                                        PluginError& error);
    static void exit(Native& native);
    static void exitAll();

    std::shared_ptr<Native> native;
    Steinberg::IPluginFactory* pluginFactory = nullptr;
};

}
