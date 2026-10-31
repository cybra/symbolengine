// A program for the crash note tests: it opens a note, with the crash handler, makes the test module's faulting plugin
// and ends as it is told: in the middle of a call into the plugin on the main thread or the audio thread, hanging for
// the test to kill it or crashing, outside every call into a plugin, or cleanly. Before it hangs or crashes it writes
// the file named after the note with ".ready" added, so the test knows the call is under way.
//
//     symbolengine_host_crash_note <note> <module> main-hang|main-crash|audio-hang|audio-crash|outside|clean

#include <symbolengine/core/Files.h>
#include <symbolengine/host/Plugin.h>
#include <symbolengine/host/PluginCrashNote.h>
#include <symbolengine/host/Vst3Format.h>

#include "TestMainThread.h"
#include "Vst3/TestIds.h"

#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>

using namespace symbolengine;

namespace
{

void ready(const std::filesystem::path& note)
{
    auto marker = note;
    marker += ".ready";
    std::error_code ignored;
    files::writeSafely(marker, "ready", ignored);
}

}

int main(int argc, char** argv)
{
    if (argc < 4)
        return 2;

    const std::filesystem::path notePath = argv[1];
    const std::string how = argv[3];
    PluginError error;
    auto note = PluginCrashNote::open(notePath, true, error);

    if (note == nullptr)
    {
        std::fprintf(stderr, "%s\n", error.message.c_str());
        return 3;
    }

    TestMainThread main;
    Vst3Format format{{"Crash note", main.post(), main.isCurrent()}};
    PluginHost host{{main.post(), main.isCurrent(), std::chrono::seconds(2),
                     [](const std::string& what) { std::fprintf(stderr, "%s\n", what.c_str()); }}};
    host.setCrashNote(note.get());
    note->setContext("Tuesday's interview.symbol");

    auto plugin = host.make(format, Vst3Format::describe(argv[2], testIds::faulting));

    if (!main.turnUntil([&] { return plugin->state() != Plugin::State::making; }) ||
        !plugin->prepare({48000.0, 64, false}))
        return 4;

    if (how == "main-hang" || how == "main-crash")
    {
        ready(notePath);
        plugin->setParameter(how == "main-hang" ? testIds::hangOnMain : testIds::faultOnMain, 1.0);
        return 5;
    }

    if (how == "audio-hang" || how == "audio-crash")
    {
        const PluginParameterChange change{how == "audio-hang" ? testIds::hangInProcess : testIds::faultInProcess, 0,
                                           1.0};
        ready(notePath);
        std::thread([&] {
            float left[64] = {}, right[64] = {};
            float* channels[] = {left, right};
            plugin->process(channels, 2, 64, &change, 1);
        }).join();
        return 5;
    }

    // The crash is on this thread, outside every call into a plugin, with the audio thread inside one all the while.
    if (how == "outside-while-playing")
    {
        const PluginParameterChange change{testIds::hangInProcess, 0, 1.0};
        std::thread([&] {
            float left[64] = {}, right[64] = {};
            float* channels[] = {left, right};
            plugin->process(channels, 2, 64, &change, 1);
        }).detach();
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        ready(notePath);
        volatile int* volatile nowhere = nullptr;
        *nowhere = 1;
        return 5;
    }

    if (how == "outside")
    {
        ready(notePath);
        volatile int* volatile nowhere = nullptr;
        *nowhere = 1;
        return 5;
    }

    plugin->release();
    plugin.reset();
    main.settle();
    return 0;
}
