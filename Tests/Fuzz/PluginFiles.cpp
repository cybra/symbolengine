// Any bytes as what a scan reads from outside a plugin's code: a list of plugins left by an earlier run, which damage costs
// only the entries it touches, and what it keeps reads back the same; and a VST3 module's manifest, read before any of
// its code. The module here has no code, so nothing can be loaded whatever the manifest says.

#include "Fuzz.h"

#include <symbolengine/host/PluginList.h>
#include <symbolengine/host/Vst3Format.h>

#include <functional>
#include <vector>

using namespace symbolengine;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    static const auto listFile = fuzz::scratch("list.json");
    static const auto bundle = [] {
        const auto made = fuzz::scratch("Module.vst3");
        std::filesystem::create_directories(made / "Contents" / "Resources");
        return made;
    }();

    fuzz::write(listFile, data, size);
    std::error_code backupGone;
    auto backup = listFile;
    backup += ".backup";
    std::filesystem::remove(backup, backupGone);

    {
        PluginList::Options options;
        options.file = listFile;
        PluginList list(options);
        const auto files = list.files();
        const auto plugins = list.plugins();
        static_cast<void>(plugins);

        std::error_code error;
        list.save(error);
        if (!error)
        {
            PluginList again(options);
            if (again.files() != files)
                fuzz::broken();
        }
    }

    fuzz::write(bundle / "Contents" / "Resources" / "moduleinfo.json", data, size);
    static Vst3Format format{{"Fuzz", [](std::function<void()> task) { task(); }, [] { return true; }}};
    PluginError problem;
    const auto found = format.pluginsIn(bundle, problem);
    static_cast<void>(found);

    return 0;
}
