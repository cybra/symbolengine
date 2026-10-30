#pragma once

#include "symbolengine/host/PluginFormat.h"

#include <functional>
#include <memory>

/** What a program's plugin windows do with each system's own windows and keys. Main thread only. */
namespace symbolengine::pluginWindowsNative
{

/** What the watch tells the plugin windows of. */
struct Calls
{
    /** A window showed, became key or closed, or the program came in front or went behind: look again. */
    std::function<void()> look;

    /** A key pressed or let go of in a window, with no modifier key held and no text being typed there: whether it is
        one of the program's from a plugin window, taken for the program. */
    std::function<bool(void* window, char32_t key, bool down, bool repeat)> take;

    /** Any other key pressed or let go of in a window, where the system offers keys to a plugin's editor before its
        own windows get them, as Windows does: whether the editor took it. */
    std::function<bool(void* window, const PluginKey& key, bool down)> offer;

    /** A beat of the main thread every beatInterval, late by as long as the thread was held up. */
    std::function<void()> beat;
};

/** How often the main thread beats. */
constexpr int beatMilliseconds = 20;

/** Watches the process's windows and its keys while it lives, telling the calls, looks every quarter second and beats
    the main thread. */
class Watch
{
public:
    explicit Watch(Calls calls);
    ~Watch();

    Watch(const Watch&) = delete;
    Watch& operator=(const Watch&) = delete;

    /** Runs the function on the main thread, once what is running there now is over. */
    void post(std::function<void()> function);

    struct Native;

private:
    std::unique_ptr<Native> native;
};

/** Whether the program is in front of other programs. */
bool inFront();

/** Whether a window of the process shows that isn't one of those known, leaving out menus, tooltips and the like. */
bool otherShowing(const std::function<bool(void* window)>& known);

/** Whether the window is the one with the keyboard, as its NSWindow or HWND. */
bool hasKeyboard(void* window);

/** Whether text is being typed in the window: in a system text field, in a view taking typed text with a place in it to
    type at or marked text, or, on Windows, in an edit control, the only kind there that can be told apart. */
bool typingIn(void* window);

}
