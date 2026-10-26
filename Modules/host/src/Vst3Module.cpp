#include "Vst3Module.h"

#include <cstdlib>
#include <map>
#include <mutex>
#include <string>

namespace symbolengine
{

namespace
{

// Every module this process has loaded, by its location, held until the program ends.
struct Registry
{
    std::mutex lock;
    std::map<std::string, std::shared_ptr<Vst3Module>> open;
};

Registry& registry()
{
    static auto* const modules = new Registry;   // never destroyed: modules outlive static destruction
    return *modules;
}

std::string keyFor(const std::filesystem::path& location)
{
    std::error_code ignored;
    const auto real = std::filesystem::weakly_canonical(location, ignored);
    return (real.empty() ? location : real).generic_string();
}

}

std::shared_ptr<Vst3Module> Vst3Module::open(const std::filesystem::path& location, PluginError& error)
{
    auto& modules = registry();
    const auto key = keyFor(location);
    const std::lock_guard held(modules.lock);

    if (const auto found = modules.open.find(key); found != modules.open.end())
        return found->second;

    std::shared_ptr<Vst3Module> module(new Vst3Module);
    module->native = load(location, module->pluginFactory, error);

    if (module->native == nullptr)
        return nullptr;

    // Registered after the module's own static objects were made, so it runs before they are taken down.
    static const bool exitsAtEnd = std::atexit(exitAll) == 0;
    static_cast<void>(exitsAtEnd);
    modules.open[key] = module;
    return module;
}

void Vst3Module::exitAll()
{
    auto& modules = registry();
    const std::lock_guard held(modules.lock);

    for (auto& [location, module] : modules.open)
    {
        if (module->plugins > 0)
            continue;

        if (module->pluginFactory != nullptr)
            module->pluginFactory->release();

        module->pluginFactory = nullptr;
        exit(*module->native);
    }
}

}
