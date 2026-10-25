# SymbolEngine

An open-source C++ framework for audio applications and plugins, free for any use, open or closed source, with no revenue limits, seats or splash screens. It builds plugins in every format, hosts other makers' plugins, runs audio devices and gives desktop apps web-page interfaces. Its first user is Symbol Studio, a commercial audio app in development, so every part is proven in a real product before it is called done.

Status: building the foundations. The first release comes when Symbol Studio runs on SymbolEngine alone. Its license is MIT, its code is at [github.com/cybra/symbolengine](https://github.com/cybra/symbolengine), and its home will be [symbolengine.dev](https://symbolengine.dev). SymbolEngine is a product of Cyberhost LLC.

## Using it

Each module is a CMake target. A project takes SymbolEngine in with FetchContent:

```cmake
include(FetchContent)
FetchContent_Declare(SymbolEngine
    GIT_REPOSITORY https://github.com/cybra/symbolengine.git
    GIT_TAG        main)   # until the first release; then a release's tag
FetchContent_MakeAvailable(SymbolEngine)

target_link_libraries(my_app PRIVATE SymbolEngine::core)
```

or, once it is installed, with `find_package(SymbolEngine REQUIRED COMPONENTS core)`.

```cpp
#include <symbolengine/core.h>

using namespace symbolengine;

SettingsFile settings(paths::systemFolder(paths::Folder::appData) / "My App" / "settings.json");
settings.set("rate", 48000);

const auto project = json::parse(files::read("take.json"));
const auto path = project["sources"][0]["path"].stringOr("");
```

The plugin module is taken in with FetchContent, since it builds plugins with a CMake function of its own:

```cmake
symbolengine_add_plugin(Gain
    SOURCES           Gain.cpp
    OUTPUT_NAME       "Gain"
    BUNDLE_IDENTIFIER com.example.gain
    FORMATS           CLAP VST3 AUV2
    AU_SUBTYPE        Gain
    AU_MANUFACTURER_CODE Exmp)
```

[Examples/Gain](Examples/Gain/Gain.cpp) is a whole plugin in about 40 lines, and [Examples/Filter](Examples/Filter) is a filter with an editor: knobs, mode buttons and a level meter in a web page.

A desktop program is built the same way, with `symbolengine_add_app`, which makes it an application bundle on a Mac and a program without a console on Windows, with its name, version, identifier and the kinds of file it opens. [Examples/Hello](Examples/Hello/Hello.cpp) is a whole program: one window holding a page, the Mac's menu bar, one instance at a time, and the files the system asks it to open.

```cpp
int main(int argc, char** argv)
{
    Application app({.name = "Editor", .identifier = "com.example.editor"}, argc, argv);
    if (app.sendToRunningInstance())   // a second instance hands over its arguments and ends
        return 0;

    ui::Window window({.title = "Editor"});
    app.onQuitRequested = [&] { app.quit(); };
    app.onOpenFiles = [&](std::vector<std::filesystem::path> files) { /* open them */ };
    window.setVisible(true);
    return app.run();
}
```

It needs CMake 3.25 or later and a C++20 compiler, and runs on macOS 11 or later, Windows 10 or later, and Linux.

## Modules

| Module | Now |
|---|---|
| core | Values with JSON, XML documents, UTF-8 text, paths and the system's folders, files written safely, settings files, SHA-256, speaker layouts, a lock-free fifo between two threads and a guard against denormal numbers |
| files | Audio files read and written a stretch at a time, at any length: WAV, with RF64 past 4 GB, its broadcast extension and every other chunk kept, AIFF and AIFF-C, and FLAC, Ogg Vorbis and Opus with their tags, written from an audio device's thread too, flushed every second so a crash costs at most the last second; MP3 read gaplessly, and M4A, AAC and CAF read through the system's own decoders |
| devices | Streams on audio devices, written once over a narrow layer for each system, with a scripted fake system for tests: device lists read without opening a device, setups saved and found again direction by direction, each stream's control thread and events in order, blocks with every lost frame counted in their positions, and dropouts, stalls, sleep and a device changing or lost under a stream all said; Core Audio on a Mac, with Windows Audio and ASIO to come |
| ui | The message loop, with calls from any thread and timers; pages in the system's own web view, WKWebView on a Mac and WebView2 on Windows, served from files built into the program, calling native functions and receiving events through a script every page is given; a program's own windows to hold them, the system's own dialogs, and the Mac's menu bar |
| app | A desktop program on a Mac or Windows: its arguments as UTF-8, the system asking it to quit, to open files or to come back, one instance at a time with a second handing over its arguments, the Dock icon, the recent documents, and `symbolengine_add_app`, which builds the application bundle and the Windows program with their version information |
| plugin | Plugins written once as a class, with parameters, state, buses from mono to 9.1.6, latency and tail, and an editor as a web page on Mac and Windows; built as CLAP, VST3 and Audio Units, keeping the class ID and codes of a plugin released before, and checked by CLAP's validator, Steinberg's, auval and pluginval |

The [architecture](Docs/02_architecture.md) describes what is to come: the devices module's systems, the host module, and the rest of files, ui and app.

## What a product includes

A product built on SymbolEngine includes these, each under a permissive license whose notice goes with the product's other notices:

| Part | Used by | License |
|---|---|---|
| SymbolEngine | everything | MIT |
| CHOC, with settings added for WebView2's data folder and background, the keyboard passed on to its page, and functions bound before a page loads | ui | ISC |
| Microsoft's WebView2Loader, carried inside CHOC | ui, on Windows | BSD-style, Microsoft's |
| CLAP SDK, and clap-wrapper from SymbolEngine's fork at github.com/cybra/clap-wrapper | plugin | MIT |
| VST3 SDK | plugin, VST3 builds | MIT |
| Apple's AudioUnitSDK | plugin, Audio Unit builds | Apache 2.0 |
| Xiph.Org's libogg, libvorbis, libFLAC, libopus, opusfile and libopusenc | files | BSD, Xiph.Org's |
| minimp3 | files | CC0, in the public domain |

## Docs

- [Vision](Docs/01_vision.md): why it exists, who it is for, what it promises, its license, platforms and name.
- [Architecture](Docs/02_architecture.md): its modules, what each is built on, its rules for threads and real time, and what real plugins and devices have taught.
- [ToDo](Docs/ToDo.md): where things stand, the open questions and the phases.
- [Upstream reports](Docs/Upstream/README.md): bugs found in the libraries and tools SymbolEngine builds on, written up for their own trackers.
- [Contributing](CONTRIBUTING.md): building, testing, the rules for code, and signing off.
