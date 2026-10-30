#include "Vst3Module.h"

#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace symbolengine
{

namespace
{

// Every module this process has loaded, by its location, held until the program ends.
struct Registry
{
    std::mutex lock;
    std::map<std::string, std::shared_ptr<Vst3Module>> open;

    // Those not yet exited, the newest last.
    std::vector<std::shared_ptr<Vst3Module>> waiting;
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

    // Registered after this module's own static objects were made, so its exit comes before they are taken down, as the
    // program's end runs what was registered last first: one for each module, as those of a later one came later.
    std::atexit(exitNewest);
    modules.open[key] = module;
    modules.waiting.push_back(module);
    return module;
}

void Vst3Module::exitNewest()
{
    auto& modules = registry();
    const std::lock_guard held(modules.lock);

    if (modules.waiting.empty())
        return;

    const auto module = std::move(modules.waiting.back());
    modules.waiting.pop_back();

    if (module->plugins > 0)
        return;

    if (module->pluginFactory != nullptr)
        module->pluginFactory->release();

    module->pluginFactory = nullptr;
    exit(*module->native);
}

}
