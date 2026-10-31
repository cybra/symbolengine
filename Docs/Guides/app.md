# Desktop programs

The app module is a desktop program's link to the system. It starts the system's application object and the message loop, gives you your arguments as UTF-8, passes on what the system asks of a running program, and can keep one instance running at a time. It runs on a Mac and on Windows, and brings the [ui module](ui.md) with it for windows and pages.

```cmake
add_executable(my_app main.cpp)
symbolengine_add_app(my_app
    NAME "My App"
    IDENTIFIER com.example.myapp
    VERSION 1.2.0)
```

`symbolengine_add_app` links `SymbolEngine::app`, and makes the target an application bundle on a Mac and a program with no console on Windows. Take SymbolEngine in with FetchContent, as the [README](../../README.md#using-it) shows, since `find_package` doesn't bring this function. [`Examples/Hello`](../../Examples/Hello/Hello.cpp) is a whole program in under 70 lines, built with [this CMake](../../Examples/Hello/CMakeLists.txt): one window holding a page, the Mac's menu bar, one instance at a time, and the files the system asks it to open.

## A program's main

An `Application` comes first, on the main thread, before any window. Set the callbacks you want, make your windows, and return what `run` gives:

```cpp
#include <symbolengine/app.h>
#include <symbolengine/ui/Window.h>

using namespace symbolengine;

int main(int argc, char** argv)
{
    Application app({.name = SYMBOLENGINE_APP_NAME, .identifier = "com.example.myapp"}, argc, argv);

    ui::Window window({.title = SYMBOLENGINE_APP_NAME});
    window.onCloseButton = [&app] { app.requestQuit(); };

    window.setVisible(true);
    return app.run();
}
```

`run` runs the message loop until `quit`, then returns the code given to it. The windows are destroyed after that, while the `Application` is still alive, which declaring them after it gives you. `symbolengine_add_app` defines `SYMBOLENGINE_APP_NAME` and `SYMBOLENGINE_APP_VERSION` as strings. There is one `Application` per program, and `Application::current()` gives it to code that wasn't handed it.

## Quitting

`quit` stops the loop, from any thread, and `run` returns its code once what is queued has run. `requestQuit` asks the program the way the system does, through `onQuitRequested`, and quits at once if there is no handler.

So send every way of quitting through `requestQuit`, and answer them in one place. The system asks too, through the same callback: Quit in the Dock's menu, logging out or shutting down on a Mac, and the session ending on Windows.

```cpp
app.onQuitRequested = [&] {
    if (!document.edited())
    {
        app.quit();
        return;
    }

    ui::ask({.title = "Save your changes?", .buttons = {"Save", "Don't Save", "Cancel"}, .cancel = 2},
            window.nativeHandle(),
            [&](std::size_t chosen) {
                if (chosen == 2)
                    return;
                if (chosen == 0)
                    document.save();
                app.quit();
            });
};
```

`isQuitting` says whether `quit` has been called, and `run` returns at once if it was called before. `Application::terminate` ends the process at once with nothing destroyed, as for a helper whose message thread may be held by what it is running.

## Opening files

`onOpenFiles` hears the files the system asks the program to open: double-clicked, dropped on its icon, or given to it with Open With. Set it before `run`:

```cpp
app.onOpenFiles = [&document](std::vector<std::filesystem::path> files) {
    for (const auto& file : files)
        document.open(file);
};
```

On a Mac that includes the file the program was started for. On Windows that file comes in `arguments()`, so read it there:

```cpp
#if defined(_WIN32)
for (const auto& argument : app.arguments())
    document.open(paths::fromUtf8(argument));
#endif
```

`arguments()` gives the arguments as UTF-8, without the program's own path. Windows' are read from the command line as the system has it, so names outside the system's code page arrive whole. `paths::fromUtf8` turns one into a path.

`DOCUMENT_EXTENSIONS` in `symbolengine_add_app` lists the kinds of file the Finder opens with the program. On Windows the function registers nothing, so that is your installer's to do.

`noteRecentFile` adds a file to the system's list of recent documents: the Dock menu's and Finder's on a Mac, Recent and the taskbar's jump list on Windows.

```cpp
app.noteRecentFile(file);
```

## One instance at a time

A program that runs one instance at a time calls `sendToRunningInstance` as it starts, before it makes a window. If an instance is already running it takes the arguments and this one returns from `main`. Otherwise this one becomes the running instance others find:

```cpp
if (app.sendToRunningInstance())
    return 0;

app.onAnotherInstance = [&](std::vector<std::string> arguments) {
    for (const auto& argument : arguments)
        document.open(paths::fromUtf8(argument));
    window.toFront();
};
```

Instances find each other by `identifier`, so give it the name you give `IDENTIFIER` in CMake. Left empty, it is the program's name. The running instance holds a lock the system lets go of however the process ends, so a crash leaves nothing in the way of the next start. A program run several times at once, as for checks from a script, leaves the call out.

## The Dock, and being in front

`onReopen` hears a click on a Mac's Dock icon while none of the program's windows show. A program whose close button only hides its window brings it back here:

```cpp
window.onCloseButton = [&window] { window.setVisible(false); };
app.onReopen = [&window] { window.toFront(); };
```

`isForeground` says whether the program is in front, with its windows the ones that have the keyboard. `setDockIconVisible(false)` takes the program out of the Dock and the application switcher on a Mac, as for a helper nobody should see, and does nothing elsewhere.

## What `symbolengine_add_app` makes

On a Mac it writes an application bundle's Info.plist from its arguments, and on Windows the version information a file's Properties show. Your own `main` is the entry on both:

```cmake
add_executable(my_app main.cpp)
symbolengine_add_app(my_app
    NAME "My App"
    IDENTIFIER com.example.myapp
    VERSION 1.2.0
    COMPANY "Example Inc."
    COPYRIGHT "Copyright Example Inc."
    ICON_NAME AppIcon
    DOCUMENT_EXTENSIONS txt md
    MICROPHONE_TEXT "My App records from the microphone you choose."
    PLIST_ENTRIES "<key>LSApplicationCategoryType</key><string>public.app-category.music</string>")

set_target_properties(my_app PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/App)
```

- **`NAME`** is what the system shows, and the file's name. It is required.
- **`IDENTIFIER`** is the bundle identifier. It is required.
- **`VERSION`** is numbers separated by dots, the project's if not given. A project with no version has to give one.
- **`COMPANY`** goes in Windows' version information, and **`COPYRIGHT`** there and in the Info.plist.
- **`ICON_NAME`** names the icon you put in the bundle's Resources yourself, as `AppIcon.icns` or an icon compiled by `actool`. The function adds no icon of its own.
- **`DOCUMENT_EXTENSIONS`** lists the kinds of file the program opens, without their dots.
- **`MICROPHONE_TEXT`** is what a Mac shows when the program first asks for the microphone.
- **`PLIST_ENTRIES`** is further XML for the Info.plist's dictionary.

Where the program lands is yours to choose with `RUNTIME_OUTPUT_DIRECTORY`, as for any executable. The Info.plist's path is in the target's `SYMBOLENGINE_APP_INFO_PLIST` property, for a build step that depends on it. Its smallest system is `CMAKE_OSX_DEPLOYMENT_TARGET`, or macOS 11 if that isn't set. Signing and notarizing the bundle are steps of your own build.

A page goes into the program with `symbolengine_add_files`, as the [ui guide](ui.md#a-page) shows.

## What's in the reference

| Where | What it gives you |
|---|---|
| `Application.h`, or `<symbolengine/app.h>` | `Application`: its arguments, its callbacks, quitting and the other instances |
| `cmake/SymbolEngineApps.cmake` | `symbolengine_add_app`, with every argument described |

The [architecture's applications section](../02_architecture.md#applications) says how each part works on each system.
