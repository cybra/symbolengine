// What the editor tests do with the system's windows on a Mac. The windows are never shown, so nothing appears on the
// display of the person running the tests.

#include <symbolengine/host/PluginWindow.h>

#import <AppKit/AppKit.h>

#include <cmath>

// Turns the system's run loop until nothing more is waiting in it, so what a window posted to it runs.
void turnSystemLoop()
{
    for (int i = 0; i < 20 && CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.0, true) == kCFRunLoopRunHandledSource; ++i)
    {
    }
}

// The user dragging the window's bottom right corner to a size: the system asks the window what size it takes, as at
// each step of a drag, and resizes it to that, its top left staying where it is. Gives its size after.
symbolengine::ui::Size dragWindow(symbolengine::PluginWindow& pluginWindow, symbolengine::ui::Size to)
{
    @autoreleasepool
    {
        NSWindow* window = static_cast<NSWindow*>(pluginWindow.nativeHandle());
        auto taken = [window frameRectForContentRect:NSMakeRect(0, 0, to.width, to.height)].size;
        id<NSWindowDelegate> delegate = window.delegate;

        if ([delegate respondsToSelector:@selector(windowWillResize:toSize:)])
            taken = [delegate windowWillResize:window toSize:taken];

        auto frame = window.frame;
        frame.origin.y = NSMaxY(frame) - taken.height;
        frame.size = taken;
        [window setFrame:frame display:NO];

        const auto content = [window contentRectForFrameRect:window.frame];
        return {static_cast<int>(std::lround(content.size.width)), static_cast<int>(std::lround(content.size.height))};
    }
}

// Keeps the window object, so a test can look at it after the program lets go of it, until letGoOfWindow.
void* keepWindow(void* window)
{
    return [static_cast<NSWindow*>(window) retain];
}

void letGoOfWindow(void* window)
{
    [static_cast<NSWindow*>(window) release];
}

// Whether the window still holds the views put in it.
bool holdsViews(void* window)
{
    NSView* content = static_cast<NSWindow*>(window).contentView;
    return content != nil && content.subviews.count > 0;
}

// A move by the user, on a Mac, needs nothing of the test: the system tells a window's scale once the move is over.
void beginWindowMove(void*) {}
void endWindowMove(void*) {}

// The system's notice that the window's backing scale changed, as when it is moved to a screen of another.
void rescaleWindow(void* window, double)
{
    [[NSNotificationCenter defaultCenter] postNotificationName:NSWindowDidChangeBackingPropertiesNotification
                                                        object:static_cast<NSWindow*>(window)];
}

// The plugin's own views in the window's view the editor is put in.
static NSView* editorView(void* window)
{
    return static_cast<NSWindow*>(window).contentView.subviews.firstObject;
}

int viewsInEditor(void* window)
{
    return static_cast<int>(editorView(window).subviews.count);
}

// The size of the first view the plugin put in it, and whether it lies at its top left.
symbolengine::ui::Size ownViewSize(void* window)
{
    const auto size = editorView(window).subviews.firstObject.frame.size;
    return {static_cast<int>(std::lround(size.width)), static_cast<int>(std::lround(size.height))};
}

bool ownViewAtTopLeft(void* window)
{
    NSView* container = editorView(window);
    const auto frame = container.subviews.firstObject.frame;
    return frame.origin.x == 0 && NSMaxY(frame) == NSHeight(container.bounds);
}
