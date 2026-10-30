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

bool floatsAbove(void* window)
{
    return static_cast<NSWindow*>(window).level == NSFloatingWindowLevel;
}

// A window of the process's own, as a question or a plugin's own window is, shown far off every display, where macOS
// leaves a window without a title bar, never taking the keyboard.
void* showOtherWindow()
{
    NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(-30000, -30000, 200, 100)
                                                   styleMask:NSWindowStyleMaskBorderless
                                                     backing:NSBackingStoreBuffered
                                                       defer:NO];
    window.releasedWhenClosed = NO;
    [window orderFront:nil];
    return window;
}

// A window at a menu's level, as a menu or a tooltip shows.
void* showMenuWindow()
{
    auto* window = static_cast<NSWindow*>(showOtherWindow());
    window.level = NSPopUpMenuWindowLevel;
    return window;
}

void closeOtherWindow(void* window)
{
    [static_cast<NSWindow*>(window) close];
    [static_cast<NSWindow*>(window) release];
}

// A key pressed and let go of in the window, as the system hands the program its events.
void pressKey(symbolengine::PluginWindow& pluginWindow, char32_t key, bool repeat, bool command)
{
    NSWindow* window = static_cast<NSWindow*>(pluginWindow.nativeHandle());
    NSString* characters =
        [[[NSString alloc] initWithBytes:&key length:sizeof key encoding:NSUTF32LittleEndianStringEncoding]
            autorelease];
    for (const auto type : {NSEventTypeKeyDown, NSEventTypeKeyUp})
        [NSApp sendEvent:[NSEvent keyEventWithType:type
                                                location:NSZeroPoint
                                           modifierFlags:command ? NSEventModifierFlagCommand : 0
                                               timestamp:0
                                            windowNumber:window.windowNumber
                                                 context:nil
                                              characters:characters
                             charactersIgnoringModifiers:characters
                                               isARepeat:repeat
                                                 keyCode:key == U' ' ? 49 : 0]];
}

// A text field in the window being typed in.
void* typeIn(symbolengine::PluginWindow& pluginWindow)
{
    NSWindow* window = static_cast<NSWindow*>(pluginWindow.nativeHandle());
    NSTextView* field = [[NSTextView alloc] initWithFrame:NSMakeRect(0, 0, 100, 20)];
    [editorView(window) addSubview:field];
    [window makeFirstResponder:field];
    return field;
}

void stopTyping(void* field)
{
    NSTextView* view = static_cast<NSTextView*>(field);
    [view.window makeFirstResponder:nil];
    [view removeFromSuperview];
    [view release];
}

// A key a plugin's view hands up to its superview, the window's own view, as views that don't use a key do: whether
// that view took it rather than passing it on, as an NSView does, to be refused with a beep.
bool handUp(symbolengine::PluginWindow& pluginWindow, char32_t key)
{
    NSWindow* window = static_cast<NSWindow*>(pluginWindow.nativeHandle());
    NSView* container = editorView(window);
    NSString* characters =
        [[[NSString alloc] initWithBytes:&key length:sizeof key encoding:NSUTF32LittleEndianStringEncoding]
            autorelease];
    NSEvent* event = [NSEvent keyEventWithType:NSEventTypeKeyDown
                                      location:NSZeroPoint
                                 modifierFlags:0
                                     timestamp:0
                                  windowNumber:window.windowNumber
                                       context:nil
                                    characters:characters
                   charactersIgnoringModifiers:characters
                                     isARepeat:NO
                                       keyCode:0];
    return
        [container methodForSelector:@selector(keyDown:)] != [NSView instanceMethodForSelector:@selector(keyDown:)] &&
        ([container keyDown:event], true);
}
