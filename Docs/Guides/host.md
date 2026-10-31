# Hosting plugins

The host module loads other makers' plugins into your program and plays audio through them: VST3 on Mac, Windows and Linux, and Audio Units v2 on a Mac. Plugins misbehave in many ways, and most of this module exists so that those ways don't become your program's problem.

```cmake
target_link_libraries(my_app PRIVATE SymbolEngine::host)
```

[`Examples/PluginPlayer`](../../Examples/PluginPlayer/PluginPlayer.cpp) is a whole program in under 200 lines, built and run as a test on every system. Everything below is about what it does, and what a larger program does beyond it.

## Threads

A plugin has two threads that matter. Everything except `Plugin::process` belongs to your main thread, and `process` belongs to your audio thread. A call on the wrong thread, or in the wrong state, is refused and told to `PluginHost::Options::misuse`; without one, a debug build stops there so you see it at once.

The host needs two things from you: how to run something on the main thread, and how to tell if it's on it. In a program with a message loop those are `ui::messageLoop::callAsync` and `ui::messageLoop::isMessageThread`:

```cpp
PluginHost host{{ui::messageLoop::callAsync, ui::messageLoop::isMessageThread, 2s, nullptr}};
Vst3Format vst3{{"My App", ui::messageLoop::callAsync, ui::messageLoop::isMessageThread}};
```

Make the host on the main thread, as that readies the thread for plugins. Keep both for as long as any plugin lives.

## Making and playing a plugin

`make` returns straight away and makes the plugin later, on the main thread, one plugin per turn of its run loop, so a slow plugin never holds up your windows. `onChange` tells you when it is ready, or why it failed:

```cpp
const auto plugin = host.make(vst3, description);

plugin->onChange = [plugin = plugin.get()](PluginChange change) {
    if (change == PluginChange::made && plugin->state() == Plugin::State::inactive)
        plugin->prepare({48000.0, 512, false});
};
```

Once it's prepared, play it from your audio thread, in place, with as many channels as you have:

```cpp
plugin->process(channels, channelCount, frames);
```

The plugin's layout is fitted to your channels. A block longer than the plugin was prepared for is split, a block that arrives while the plugin is being reset or given a state is silent rather than heard unprocessed, and once a plugin has failed the audio passes it by.

The last hold on a plugin can be let go of on any thread. It is taken down on the main thread, once no call into it is under way.

A plugin settles on stereo where it takes it, then mono in and stereo out, then mono. To play one through its own mono path instead, ask while it isn't prepared, and use what it gives back, as a plugin may not take what you asked for:

```cpp
const auto settled = plugin->arrange({1, 1});
```

## Parameters and state

Values run from 0 to 1 in every format. `setParameter` reaches the plugin's processing at its next block and shows in its editor at once:

```cpp
for (const auto& parameter : plugin->parameters())
    if (parameter.name == "Gain")
        plugin->setParameter(parameter.id, 0.75);
```

For automation, pass the block's changes to `process`, in order of their frames, and each is heard at its frame. Pass a `PluginTime` too if the program has a position and tempo.

When the plugin moves a parameter itself, `onEdit` tells you, and whether it was the user's doing, which is when your document has changed.

`saveState` and `loadState` give and take the plugin's state as a string, once its processing has heard every value set:

```cpp
const auto state = plugin->saveState(PluginStateUse::document);
```

## Finding plugins

Never look inside plugin files in your program's own process. A plugin that crashes as it loads would take your program with it, on every start. A scan runs in worker processes instead, copies of your program started in a worker mode, so a plugin that crashes or hangs takes down only a worker and is set aside with the reason.

The first thing `main` does is ask whether it's a worker, before your settings, logging or anything else:

```cpp
int main(int argc, char** argv)
{
    if (PluginScanWorker::isWorker(argc, argv))
    {
        Vst3Format vst3({"My App", PluginScanWorker::post, PluginScanWorker::onMainThread});
        PluginScanWorker::work({&vst3});
    }

    // The program itself.
}
```

Then a `PluginList` keeps what was found, in a file of your choosing, and a `PluginScanner` fills it. `walk` finds what needs looking at in each format's folders, and `scan` looks at those files, a worker at a time, recording each as it goes. A scan the user quits partway carries on next time:

```cpp
const auto walked = PluginScanner::walk(folders, list);
scanner.scan(walked.files);
```

The list says of each file what was found in it, and of a file set aside what went wrong and at which step. It also lists what the system won't run, such as a downloaded plugin that isn't notarized or code that no longer matches its signature, without loading any of it.

## Editors

`PluginWindow` opens a plugin's editor in a window of your program's, the right size for it, following it as it resizes:

```cpp
PluginError error;
auto window = PluginWindow::open(plugin, {}, error);

if (window != nullptr)
    window->toFront();
```

Closing the window destroys the editor, and opening it again makes a new one. A plugin with no editor, or one whose editor can't be opened, gets a page of its parameters instead, and `PluginWindow::Options::page` lets you give your own page for that.

Make one `PluginWindows` and give it to each window through `PluginWindow::Options::windows`. It keeps plugin windows above your program's while your program is in front, gets them out of the way of your own questions and file choosers, takes your program's keys from them, Space unless you name others, and tells you if an editor holds up the main thread.

## When a plugin fails

An exception from a plugin never reaches your program: the plugin is switched off, `problem()` says why, and `onChange` tells you with `PluginChange::failed`. A plugin that doesn't leave a process call within `audioLimit` is left out of the audio and never called again, `PluginChange::stuck`. A plugin whose output holds a value that isn't a number, from audio that held none, has that block played as it came, as those after it are while it is reset, and it then fades back in; `PluginChange::notFinite` says so and `notFiniteBlocks()` counts them, and one that goes on doing it is switched off.

A crash differs by system. On Windows every call into a plugin is guarded, so a crash switches the plugin off and `PluginChange::crashed` says which call. Your process can no longer be trusted by then, so save the document to a new file and ask the user to restart. On a Mac and Linux a crash ends the program.

So keep a crash note. It tells your next run which plugin and call the last run ended in, however it ended:

```cpp
PluginError error;
auto note = PluginCrashNote::open(settingsFolder / "Plugin Note", true, error);
host.setCrashNote(note.get());

if (const auto* culprit = note->lastRun().culprit())
    offerToLeaveOut(culprit->plugin, culprit->call);
```

Open it as your program starts, before any plugin is made, and keep it for longer than the host. The note costs a few tens of billionths of a second a call, so it can stay on all the time.

## Timing

`Plugin::timing` says how long a plugin's blocks take against the time they last. `takeTiming` also starts its load and worst block again, for a meter showing the worst since it last looked:

```cpp
const auto timing = plugin->takeTiming();
meter.show(timing.load, timing.worstShare);
```

`PluginHost::slowestBetween` names the plugin that made a device's block late. Give it to the stream through `AudioStream::Options::describeLate`, and a late block's event says which plugin to blame:

```cpp
options.describeLate = [&host](std::int64_t from, std::int64_t to) -> std::string {
    const auto late = host.slowestBetween(from, to);
    return late ? late->plugin.name : std::string{};
};
```

## Finding what a plugin shouldn't call

A plugin that allocates memory, takes a lock or reaches for a file on the audio thread plays perfectly until the moment it doesn't. A debug mode finds those calls and names the plugin, with the audio playing on:

```cpp
PluginHost::Options options{post, onMainThread, 2s, nullptr};
options.watchUnsafeCalls = true;
```

On Windows that's all it needs. On a Mac and Linux the program also links a small library, which has to be there as the program starts, and costs about 5 billionths of a second on each such call in the whole process, so it belongs in a debug build:

```cmake
target_link_libraries(my_app PRIVATE SymbolEngine::host_unsafe_calls)
```

Or leave your program as it is and have the system insert the library as it starts, with `DYLD_INSERT_LIBRARIES` on a Mac or `LD_PRELOAD` on Linux. Either way, `PluginHost::canWatchUnsafeCalls` tries it for real and says whether it works. Loading the library later doesn't.

Then read what was found:

```cpp
for (const auto& call : plugin->unsafeCalls())
    if (call.inPluginCode)
        log(call.function, call.step, call.firstBlock, call.lastBlock, call.backtrace);
```

- **`firstBlock` and `lastBlock`** tell set-up from trouble. Plugins often allocate once in their first block, and the system binds what a plugin calls for the first time, so a lock's first use makes several calls where later ones make one. One made in block 0 alone is rarely worth worrying about. One that goes on to the last block is.
- **`step`** says which call into the plugin it came during, `process`, or `startProcessing` as processing begins.
- **`inPluginCode`** says whether the plugin's own code is on the way to it, as it is when a system library does something the plugin asked for. Where it isn't, the call came from the system's code or your own on the way into the plugin. A Debug build of your program on Windows shows `EnterCriticalSection` this way in every block, as Microsoft's debug runtime locks for every checked iterator made, which a release build doesn't.

Only real-time play is watched. An offline render isn't, as nothing is waiting for it and a plugin may rightly wait there.

It can't see into a plugin running in a process of its own, as an Intel-only Audio Unit does through Rosetta on Apple Silicon, or what a plugin's own threads do during its call, which isn't held against it. On Windows it doesn't see what a module looks up by name as it runs.

For your own plugin's code, clang's real-time checker (`-fsanitize=realtime`) is also worth using on a Mac and Linux.

## What's in the reference

| Header | What it gives you |
|---|---|
| `Plugin.h` | `PluginHost`, `Plugin`, `PluginEditor`, and what a plugin tells you |
| `PluginFormat.h` | `PluginDescription`, `PluginSetup`, `PluginLayout`, `PluginParameter` |
| `Vst3Format.h`, `AudioUnitFormat.h` | Each format, and where its plugins live |
| `PluginList.h`, `PluginScanner.h`, `PluginScanWorker.h` | Scanning in workers, and what it found |
| `PluginWindow.h`, `PluginWindows.h` | A plugin's window, and all of a program's together |
| `PluginCrashNote.h` | What the last run ended in |
| `FakePluginFormat.h` | A format for your own tests, whose plugins check every call made into them and misbehave as you script them to |

The [architecture's hosting section](../02_architecture.md#hosting) holds every problem hosts have met with these formats and what this module does about each.
