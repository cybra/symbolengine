#include "symbolengine/host/PluginWindows.h"

#include "symbolengine/host/PluginWindow.h"

#include "PluginWindowsNative.h"

#include <algorithm>
#include <set>

namespace symbolengine
{

struct PluginWindows::Impl
{
    Options options;
    std::vector<PluginWindow*> windows;
    bool floats = false;

    // Keys taken as they were pressed, whose letting go is taken too.
    std::set<char32_t> held;

    std::unique_ptr<pluginWindowsNative::Watch> watch;

    PluginWindow* windowOf(void* native) const
    {
        const auto found = std::find_if(windows.begin(), windows.end(),
                                        [native](PluginWindow* window) { return window->nativeHandle() == native; });
        return found != windows.end() ? *found : nullptr;
    }

    // The program's windows and its plugins', which plugin windows never step back for.
    bool known(void* native) const
    {
        return (options.program != nullptr && options.program->nativeHandle() == native) ||
               (options.isProgramWindow != nullptr && options.isProgramWindow(native)) || windowOf(native) != nullptr;
    }

    bool inFront() const { return options.inFront != nullptr ? options.inFront() : pluginWindowsNative::inFront(); }

    void arrange(bool always)
    {
        const bool wanted =
            inFront() && !pluginWindowsNative::otherShowing([this](void* native) { return known(native); });

        if (wanted == floats && !always)
            return;

        floats = wanted;

        for (auto* window : windows)
            window->setAlwaysOnTop(floats);
    }

    // One of the program's keys pressed in a plugin window, taken: told once the key's event is over, once for a key
    // held down.
    bool take(void* native, char32_t key, bool down, bool repeat)
    {
        if (windowOf(native) == nullptr)
            return false;

        if (!down)
            return held.erase(key) > 0;

        if (std::find(options.keys.begin(), options.keys.end(), key) == options.keys.end())
            return false;

        held.insert(key);

        if (!repeat && options.onKey != nullptr)
            watch->post([onKey = options.onKey, key] { onKey(key); });

        return true;
    }

    bool offer(void* native, const PluginKey& key, bool down)
    {
        auto* window = windowOf(native);
        auto* editor = window != nullptr ? window->editor() : nullptr;

        if (editor == nullptr)
            return false;

        return down ? editor->keyDown(key) : editor->keyUp(key);
    }
};

PluginWindows::PluginWindows(Options options) : impl(std::make_unique<Impl>())
{
    impl->options = std::move(options);
    auto* state = impl.get();
    impl->watch = std::make_unique<pluginWindowsNative::Watch>(pluginWindowsNative::Calls{
        [state] { state->arrange(false); },
        [state](void* window, char32_t key, bool down, bool repeat) { return state->take(window, key, down, repeat); },
        [state](void* window, const PluginKey& key, bool down) { return state->offer(window, key, down); }});
    impl->floats = impl->inFront();
}

PluginWindows::~PluginWindows() = default;

bool PluginWindows::floating() const
{
    return impl->floats;
}

void PluginWindows::arrange()
{
    impl->arrange(false);
}

void PluginWindows::join(PluginWindow& window)
{
    impl->windows.push_back(&window);
    impl->arrange(true);
}

// A key a plugin's view handed up to its window's own view, taken there: the program's, if it is one.
void PluginWindows::handedUp(PluginWindow& window, char32_t key, bool repeat)
{
    impl->take(window.nativeHandle(), key, true, repeat);
    impl->held.erase(key);
}

// A window that had the keyboard gives it back to the program's.
void PluginWindows::leave(PluginWindow& window)
{
    const bool hadKeyboard = pluginWindowsNative::hasKeyboard(window.nativeHandle());
    std::erase(impl->windows, &window);

    if (hadKeyboard && impl->options.program != nullptr)
        impl->options.program->focusContent();
}

}
