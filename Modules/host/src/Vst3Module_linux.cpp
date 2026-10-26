#include "Vst3Module.h"

#include <dlfcn.h>

#include <string>

namespace symbolengine
{

struct Vst3Module::Native
{
    void* module = nullptr;
    bool (*exit)() = nullptr;
};

std::shared_ptr<Vst3Module::Native> Vst3Module::load(const std::filesystem::path& location,
                                                     Steinberg::IPluginFactory*& factory, PluginError& error)
{
#if defined(__aarch64__)
    const char* folder = "aarch64-linux";
#else
    const char* folder = "x86_64-linux";
#endif

    std::error_code ignored;
    auto binary = location;

    if (std::filesystem::is_directory(location, ignored))
        binary = location / "Contents" / folder / location.stem().concat(".so");

    if (!std::filesystem::is_regular_file(binary, ignored))
    {
        error = {"Its bundle holds no code for this computer's kind of processor.", "load", 0};
        return nullptr;
    }

    void* module = dlopen(binary.c_str(), RTLD_LAZY | RTLD_LOCAL);

    if (module == nullptr)
    {
        const char* why = dlerror();
        error = {why != nullptr ? why : "Its code couldn't be loaded.", "load", 0};
        return nullptr;
    }

    using Entry = bool (*)(void*);
    using Factory = Steinberg::IPluginFactory* (*) ();
    auto native = std::make_shared<Native>();
    native->module = module;
    const auto entry = reinterpret_cast<Entry>(dlsym(module, "ModuleEntry"));
    native->exit = reinterpret_cast<bool (*)()>(dlsym(module, "ModuleExit"));
    const auto getFactory = reinterpret_cast<Factory>(dlsym(module, "GetPluginFactory"));

    if (getFactory == nullptr)
    {
        error = {"It has no plugin factory (GetPluginFactory).", "load", 0};
        return nullptr;
    }

    if (entry != nullptr && !entry(module))
    {
        error = {"Its entry function refused to start it.", "ModuleEntry", 0};
        native->exit = nullptr;
        return nullptr;
    }

    factory = getFactory();

    if (factory == nullptr)
    {
        error = {"It gave no plugin factory.", "GetPluginFactory", 0};

        if (native->exit != nullptr)
            native->exit();

        return nullptr;
    }

    return native;
}

void Vst3Module::exit(Native& native)
{
    if (native.exit != nullptr)
        native.exit();
}

}
