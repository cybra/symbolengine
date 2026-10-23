#pragma once

#include <cstdint>
#include <functional>
#include <memory>

namespace symbolengine::ui
{

/** Calls a function on the message thread at an interval, from when it is made until it is destroyed. Made and
    destroyed on the message thread. */
class Timer
{
public:
    Timer(std::uint32_t intervalMilliseconds, std::function<void()> tick);
    ~Timer();

    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

/** Native views, such as a WebView's, placed in another program's window, as a plugin's editor is placed in a host's.
    A view is an NSView on a Mac and an HWND on Windows. Message thread only. */
namespace native
{
/** Puts the view inside the parent, at its top left. */
bool attach(void* view, void* parent);

/** Takes the view out of its parent. */
void detach(void* view);

/** Sizes the view: in points on a Mac, and in pixels on Windows. */
void resize(void* view, std::uint32_t width, std::uint32_t height);

void show(void* view, bool visible);

/** The size of the screen under the pointer, less the menu bar, the Dock and the taskbar: in points on a Mac, and in
    pixels on Windows. A window opens there when something has just been clicked to open it. */
struct Area
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};
Area workAreaAtPointer();
}

}
