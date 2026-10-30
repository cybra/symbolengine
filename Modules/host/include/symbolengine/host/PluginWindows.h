#pragma once

#include <symbolengine/ui/Window.h>

#include <chrono>
#include <functional>
#include <memory>
#include <vector>

namespace symbolengine
{

class Plugin;
class PluginWindow;

/** Every plugin window of a program, kept in their place among the program's windows, with the program's keys taken
    from them. Plugin windows float above the program's while it is in front, and step back while any other window of
    its process shows, as one of its questions or file choosers, or a window a plugin opened itself, so nothing waiting
    for an answer is hidden. Plugin windows join it as they open, through PluginWindow::Options::windows, and leave as
    they are destroyed. Made, used and destroyed on the main thread, before any window joins and after every one has
    left; built where the ui module is, on a Mac and on Windows. */
class PluginWindows
{
public:
    struct Options
    {
        /** The program's main window: plugin windows float above it while the program is in front, and it is given the
            keyboard back as a plugin window that had it closes. */
        ui::Window* program = nullptr;

        /** Whether a window, as its NSWindow on a Mac or HWND on Windows, is one of the program's own besides its main
            window, as a tool window is, which plugin windows float above too. */
        std::function<bool(void* window)> isProgramWindow;

        /** The keys taken for the program from plugin windows whatever the plugin does with keys, unless text is being
            typed there: each as its character, pressed with no modifier key. Space unless given. */
        std::vector<char32_t> keys{U' '};

        /** Told of each key taken, once the key's event is over, once for a key held down. */
        std::function<void(char32_t key)> onKey;

        /** Whether the program is in front of other programs: the system's answer unless given, as a test gives its
            own. */
        std::function<bool()> inFront;

        /** How long the main thread may be held up, while plugin windows are open, before it is said, as when an
            editor redraws for too long. */
        std::chrono::milliseconds stallLimit{100};

        /** Told once the main thread has been held up longer than the limit while plugin windows were open: how long,
            and the plugins whose windows were open, those showing first. */
        std::function<void(std::chrono::milliseconds stall, const std::vector<std::shared_ptr<Plugin>>& plugins)>
            onStall;
    };

    explicit PluginWindows(Options options);
    ~PluginWindows();

    PluginWindows(const PluginWindows&) = delete;
    PluginWindows& operator=(const PluginWindows&) = delete;

    /** Whether plugin windows float above the program's now. */
    bool floating() const;

    /** Looks again at the windows showing and whether the program is in front, as after the program has shown or
        closed a window of its own; it looks itself as windows show, become key or close, and every quarter second. */
    void arrange();

    struct Impl;

private:
    friend class PluginWindow;
    void join(PluginWindow& window);
    void leave(PluginWindow& window);
    void handedUp(PluginWindow& window, char32_t key, bool repeat);

    std::unique_ptr<Impl> impl;
};

}
