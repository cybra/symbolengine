# Windows and web pages

The ui module draws a program's interface as web pages in the system's own web view, WKWebView on a Mac and WebView2 on Windows. It serves those pages from files built into the program, and gives them windows to sit in, the system's dialogs, the Mac's menu bar, and the message loop all of it runs on. It is built on a Mac and on Windows.

```cmake
target_link_libraries(my_app PRIVATE SymbolEngine::ui)
```

Take SymbolEngine in with FetchContent, as the [README](../../README.md#using-it) shows, since `find_package` doesn't bring the CMake functions below.

[`Examples/Hello`](../../Examples/Hello/Hello.cpp) is a whole program in under 70 lines: one window holding a page, the Mac's menu bar, and the files the system asks it to open. It also uses the [app module](app.md), as most desktop programs will, which runs the message loop for it. A plugin's editor is a page too, and the [plugin module](plugin.md) makes its web view for it.

The module has no header that includes the others, so include each one you use, such as `<symbolengine/ui/Window.h>`. The code below assumes `using namespace symbolengine;`.

## The message thread

One thread runs the message loop, and everything in this module is made, used and destroyed on it. Any other thread reaches it with `callAsync`, which runs a function there after everything queued before it:

```cpp
ui::messageLoop::callAsync([&view, peak] { view.send("scanned", peak); });
```

On the message thread, `callAfter` runs a function once a time has passed. A `Timer` calls one at an interval for as long as it lives:

```cpp
ui::messageLoop::callAfter(500, [&window] { window.toFront(); });

ui::Timer meter(33, [&view, &levels] { view.send("levels", levels.read()); });
```

An `Application` runs the loop for a desktop program. A program without one runs it itself, and `stop` ends it from any thread:

```cpp
ui::messageLoop::initialize();

// Make the windows and pages here.

ui::messageLoop::run();
```

`initialize` makes this thread the message thread before `run` does, so that `callAsync`, `callAfter` and `isMessageThread` work while the program sets up.

Two calls are for harder cases. `callSoon` runs a function soon whatever the thread is doing: running a modal loop of its own, or inside a call that came through `callAsync`, whose calls wait for each other. `runUntil` turns the loop on the message thread until a condition holds or a limit passes, for a call that needs what only the loop can bring. Timers and the system's events run meanwhile, so what else the program does on this thread may run within it.

## A page

A page's files live in a folder of your source. `symbolengine_add_files` builds every file in that folder into a target, and defines a function that gives a file's bytes by its path from the folder, such as `/index.html` or `/images/knob.svg`:

```cmake
symbolengine_add_files(my_app FOLDER web FUNCTION pageFiles)
```

The code that calls the function declares it the same way, and gives the files to a `WebView`:

```cpp
std::optional<std::string_view> pageFiles(std::string_view path);

ui::WebView view({.resources =
                      [](std::string_view path) -> std::optional<ui::Resource> {
                          if (const auto file = pageFiles(path))
                              return ui::Resource{std::string(*file), ui::mimeTypeFor(path)};
                          return std::nullopt;
                      },
                  .name = "my-app"});
```

The page starts at `/index.html`. Every page is also served `symbolengine.js`, its side of the bridge, which isn't in your folder:

```html
<script src="symbolengine.js"></script>
```

`name` becomes the page's address, so pages with different names never share what they store, even in one program, and pages with the same name always do. `available()` says whether the system's web view is there to use: Windows without the WebView2 runtime has none, which it may only find out a moment after the view is made, so a program that shows something else in its place looks again once its page should have loaded. The other options are these:

- **`developerTools`** puts the browser's inspector on the page's context menu.
- **`editingKeys`** has Command-C, X, V, Z and A act on the page's text on a Mac. It is on by default. A program with those keys in its own Edit menu turns it off, so they reach the menu.
- **`dataFolder`** is where WebView2 keeps its data on Windows, which it must be able to write to. Empty means a folder named after the program in `AppData\Roaming`.
- **`background`** is the color WebView2 shows on Windows where the page hasn't drawn, as `0xRRGGBB`. Without one that shows white. A Mac's web view always shows its window's own background.
- **`allowAddress`** says whether the page, or a frame in it, may go to an address other than its own files, or open a new window there. Without it, it goes nowhere else: a link to another address does nothing, and the native functions a page is given stay with the program's own pages.

While you work on a page, `SYMBOLENGINE_LIVE_FILES=1` in the environment of a debug build reads each file from the folder it was built from, so a change shows on a reload with no build. A release build doesn't have this. A file changed is built in again by the next build, and a file added or removed is found the next time the build runs.

## Talking to the page

`expose` adds a function the page calls. It gets its arguments as an array, what it returns is what the page's promise gives, and an exception it throws rejects the promise with its message:

```cpp
view.expose("greet", [](const Value& arguments) -> Value {
    return "Hello, " + arguments[0].stringOr("you");
});
```

```js
const greeting = await symbolengine.call("greet", "Ada");
```

Values cross as JSON. `Value`, `Object` and `Array` come from the core module, and reading with `stringOr`, `numberOr`, `intOr` or `boolOr` never throws, so a missing argument reads as the fallback.

A function whose answer comes later, as after a dialog, uses `exposeLater`. The promise waits until the function calls `reply`, which it may do at any time, and only the first answer counts:

```cpp
view.exposeLater("pickFile", [&window](const Value&, ui::WebView::Reply reply) {
    ui::chooseFile({.title = "Open a file"}, window.nativeHandle(), [reply](std::optional<std::filesystem::path> chosen) {
        reply(chosen ? Value(paths::toUtf8(*chosen)) : Value());
    });
});
```

A link meant for the user's browser goes through `symbolengine.openLink`, which opens an http or https address there and gives whether the system took it:

```js
document.querySelector("#website").onclick = () => symbolengine.openLink("https://example.com/");
```

`send` sends an event to the page's handlers, and `symbolengine.on` adds one, giving back a function that removes it:

```cpp
view.send("progress", Object{{"done", 3}, {"total", 10}});
```

```js
const stop = symbolengine.on("progress", ({ done, total }) => show(done / total));
```

The native side holds what it sends or runs until the page has loaded `symbolengine.js`, so nothing sent early is lost. `pageReady()` says whether the page is taking what it sends. If the process drawing the page dies, the page is loaded again by itself, and `pageReady()` is false until it has, with what is sent meanwhile held for it. So a page loaded a second time is something a program can see: it asks for what it shows as it loads, rather than being told once.

Unless `developerTools` is on, a page behaves as a part of the program, not a browser: its context menu has nothing that leaves the page, and on Windows there is no browser menu at all, no reload key and no zooming with the wheel. A page can ask for part of a file, which is what lets an `<audio>` or `<video>` element play one built in.

`run` runs a script in the page. Given a function too, it gives what the script's last expression came to, as JSON has it, or null if it threw, and on a Mac what went wrong:

```cpp
view.run("document.title", [](const Value& result, const std::string& error) {
    show(result.stringOr(error));
});
```

For checks run from a script, `snapshot` saves a picture of the page to a PNG file and says whether it was written. That works on a Mac, and elsewhere it says false.

## Windows

A `Window` has the system's title bar and buttons, and holds one native view, which fills it. It is made hidden, in the middle of the main screen:

```cpp
ui::Window window({.title = "My App", .size = {900, 600}, .minimumSize = {480, 320}, .background = 0x1C2230});
window.setContent(view.nativeView());
window.onCloseButton = [&window] { window.setVisible(false); };

window.setVisible(true);
window.toFront();
```

The window doesn't own the view, so make the view first and it outlives the window. The close button only tells you: hide the window, destroy it, or quit, as you choose. You may destroy it from inside `onCloseButton`, which is called once the click is over.

`setVisible(true)` shows the window without taking the keyboard from the one that has it. `toFront` also brings the program in front and gives the window the keyboard.

A window's size is what it holds, without the title bar. It is in points on a Mac, and on Windows in pixels at 100% scaling, which the window turns into the pixels of the screen it is on, so it looks the same size on every screen. On Windows, `sizedInPixels` measures it in the screen's own pixels instead, for a window holding something that sizes itself in pixels.

To open a window where it was left, keep `restoredBounds`, where it was the last time it wasn't maximized or minimized, along with whether it was maximized:

```cpp
const auto place = window.restoredBounds();
settings.set("window", Object{{"x", place.x}, {"y", place.y}, {"width", place.width}, {"height", place.height}});
settings.set("maximized", window.isMaximized());
```

Give them back as it opens. `setBounds` holds the window to its smallest size and to the screen it overlaps most, so a place kept on a screen since unplugged still opens where it can be reached:

```cpp
const auto place = settings.get("window");
const auto kept = [&place](std::string_view name, int fallback) { return static_cast<int>(place[name].intOr(fallback)); };

window.setBounds({kept("x", 100), kept("y", 100), kept("width", 900), kept("height", 600)});
window.setMaximized(settings.get("maximized").boolOr(false));
```

`onResizing` lets you hold a window to sizes of your own while the user drags its edge:

```cpp
window.onResizing = [](ui::Size dragged) -> ui::Size { return {dragged.width, dragged.width * 9 / 16}; };
```

`Window.h` describes the rest. These are the ones a program most often wants:

- **`setSeeThrough`** leaves the window see-through while it shows, as for a page still drawing its first frame.
- **`setAppearance`** sets its title bar, and the look a web view in it reports to its page, to the computer's setting, light or dark.
- **`setEdited`** shows a dot in a Mac's close button while the window holds changes not yet saved.
- **`setTitleBarColor`** colors the title bar on Windows 11, and does nothing elsewhere.
- **`focusContent`** gives the view the keyboard, as after a dialog has had it.
- **`onActiveChanged`, `onMovedOrResized`, `onScaleChanged`** tell you when the window gained or lost the keyboard, moved or resized, or changed scale, the last once the user's move is over.

## Dialogs

The system's dialogs are shown once the call has returned, and answer on the message thread when they close. `ask` gives the number of the button chosen, counting from 0. Return presses the first button, and `cancel` names the one Escape presses:

```cpp
ui::ask({.title = "Save your changes?",
         .message = "They are lost if you don't.",
         .kind = ui::Question::Kind::warning,
         .buttons = {"Save", "Don't Save", "Cancel"},
         .cancel = 2},
        window.nativeHandle(),
        [&document](std::size_t chosen) {
            if (chosen == 0)
                document.save();
        });
```

Given a parent window, the question is asked in front of it, as a sheet on a Mac. Given `nullptr`, it is asked in front of the program.

`chooseFile` asks for a file to open, a folder to open, or a file to save, and gives it, or nothing if none was chosen. For saving, `start` names the file it offers, in the folder it starts in, and saving over a file asks first. A file saved without one of the extensions is given the first:

```cpp
ui::chooseFile({.mode = ui::FileChoice::Mode::save,
                .title = "Save the take",
                .start = takes / "Take 1.wav",
                .extensions = {".wav", ".flac"},
                .kindName = "Audio files"},
               window.nativeHandle(),
               [&document](std::optional<std::filesystem::path> chosen) {
                   if (chosen)
                       document.saveAs(*chosen);
               });
```

The parent window counts on Windows. On a Mac the chooser is in front of the program.

## The Mac's menu bar

`MenuBar` puts the program's menus in the Mac's menu bar, and on Windows it shows nothing, as a program there draws its menus in its own window, which a page can. Make one, on the message thread, and keep it. It takes the items for the app menu, the menus after it, and what Quit calls:

```cpp
ui::MenuBar menus(
    {{.title = "About My App", .chosen = [&view] { view.send("about", nullptr); }}},
    {{.title = "File",
      .items =
          [&document] {
              return std::vector<ui::MenuItem>{
                  {.title = "Save", .shortcut = ui::Shortcut{.key = 's'}, .enabled = document.edited(), .chosen = [&document] { document.save(); }},
                  {},
                  {.title = "Export", .submenu = {{.title = "As WAV", .chosen = [&document] { document.exportWav(); }}}}};
          }}},
    [] { ui::messageLoop::stop(); });
```

An item with no title is a line between groups. The app menu holds the items given first, then Services, Hide, Hide Others, Show All and Quit. A menu asks for its items as it opens, and again when you call `menus.changed()`, which waits until any open menu has closed, so nothing moves under the pointer.

## Placing a view in another window

`ui::native` puts a native view, an NSView on a Mac and an HWND on Windows, inside another program's window, as a plugin's editor sits in a host's. The plugin module does this for its editors, so you rarely need it yourself:

```cpp
ui::native::attach(view.nativeView(), parent);
ui::native::resize(view.nativeView(), 600, 400);
ui::native::show(view.nativeView(), true);
```

`detach` takes the view out again. `workAreaAtPointer` gives the size of the screen under the pointer, less the menu bar, the Dock and the taskbar, where a window opens when something has just been clicked to open it.

## What's in the reference

| Header | What it gives you |
|---|---|
| `MessageLoop.h` | `ui::messageLoop`: running and stopping the loop, and reaching its thread |
| `Native.h` | `Timer`, and `ui::native` for a view in another program's window |
| `WebView.h` | `WebView`, `Resource`, `Resources`, `mimeTypeFor` |
| `Window.h` | `Window`, `Size`, `Bounds`, `Appearance` |
| `Dialogs.h` | `ask` and `Question`, `chooseFile` and `FileChoice` |
| `MenuBar.h` | `MenuBar`, `Menu`, `MenuItem`, `Shortcut` |

The function that builds a page's files in is in `cmake/SymbolEngineResources.cmake`, and the page's side of the bridge is `Modules/ui/web/symbolengine.js`. The [architecture's web pages section](../02_architecture.md#web-pages) says how each part is built and why.
