// A program that loads the test module, makes and lets go of one of its plugins three times, and ends, for the test of
// a module's entry and exit coming once each.

#include <symbolengine/host/Plugin.h>
#include <symbolengine/host/Vst3Format.h>

#include "../TestMainThread.h"
#include "TestIds.h"

#include <cstdio>

using namespace symbolengine;

int main(int argc, char** argv)
{
    if (argc < 2)
        return 2;

    TestMainThread main;
    Vst3Format format{{"Module life", main.post(), main.isCurrent()}};
    PluginHost host{{main.post(), main.isCurrent(), std::chrono::seconds(2),
                     [](const std::string& what) { std::fprintf(stderr, "%s\n", what.c_str()); }}};

    for (int i = 0; i < 3; ++i)
    {
        auto plugin = host.make(format, Vst3Format::describe(argv[1], testIds::gain));

        if (!main.turnUntil([&] { return plugin->state() != Plugin::State::making; }) ||
            !plugin->prepare({48000.0, 64, false}))
            return 3;

        plugin->release();
        plugin.reset();
        main.settle();
    }

    return 0;
}
