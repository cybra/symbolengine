#include "PluginWindowsNative.h"

#import <AppKit/AppKit.h>

#include <string>

namespace symbolengine::pluginWindowsNative
{

namespace
{

// The modifier keys that make a key another.
constexpr NSEventModifierFlags modifiers =
    NSEventModifierFlagCommand | NSEventModifierFlagControl | NSEventModifierFlagOption | NSEventModifierFlagShift;

char32_t keyOf(NSEvent* event)
{
    NSString* characters = event.charactersIgnoringModifiers;

    if (characters.length == 0)
        return 0;

    char32_t key = 0;
    [characters getBytes:&key
               maxLength:sizeof key
              usedLength:nullptr
                encoding:NSUTF32LittleEndianStringEncoding
                 options:0
                   range:NSMakeRange(0, [characters rangeOfComposedCharacterSequenceAtIndex:0].length)
          remainingRange:nullptr];
    return key;
}

}

struct Watch::Native
{
    Calls calls;
    id monitor = nil;
    std::vector<id> observers;
    NSTimer* timer = nil;
    NSTimer* beats = nil;
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
};

Watch::Watch(Calls calls) : native(std::make_unique<Native>())
{
    native->calls = std::move(calls);
    auto* state = native.get();

    // The program's keys from plugin windows, taken as the program receives them, before any view of a plugin's does.
    native->monitor = [[NSEvent
        addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown | NSEventMaskKeyUp
                                     handler:^NSEvent*(NSEvent* event) {
                                       const bool down = event.type == NSEventTypeKeyDown;
                                       if (event.window == nil || (down && (event.modifierFlags & modifiers) != 0) ||
                                           (down && typingIn(event.window)))
                                           return event;
                                       const auto key = keyOf(event);
                                       return key != 0 && state->calls.take(event.window, key, down, event.isARepeat)
                                                  ? nil
                                                  : event;
                                     }] retain];

    // Windows showing, becoming key and closing, and the program coming in front or going behind: looked at again
    // once the change is over, as a closing window is still there as it says so.
    std::weak_ptr<bool> living = native->alive;
    auto* center = [NSNotificationCenter defaultCenter];
    for (NSNotificationName name : {NSApplicationDidBecomeActiveNotification, NSApplicationDidResignActiveNotification,
                                    NSWindowDidBecomeKeyNotification, NSWindowDidBecomeMainNotification,
                                    NSWindowWillCloseNotification, NSWindowDidMiniaturizeNotification})
        native->observers.push_back([[center addObserverForName:name
                                                         object:nil
                                                          queue:nil
                                                     usingBlock:^(NSNotification*) {
                                                       dispatch_async(dispatch_get_main_queue(), ^{
                                                         if (const auto still = living.lock(); still && *still)
                                                             state->calls.look();
                                                       });
                                                     }] retain]);

    // And every quarter second, for a window that shows without becoming key.
    native->timer = [[NSTimer timerWithTimeInterval:0.25
                                            repeats:YES
                                              block:^(NSTimer*) {
                                                state->calls.look();
                                              }] retain];
    [[NSRunLoop mainRunLoop] addTimer:native->timer forMode:NSRunLoopCommonModes];

    native->beats = [[NSTimer timerWithTimeInterval:beatMilliseconds / 1000.0
                                            repeats:YES
                                              block:^(NSTimer*) {
                                                state->calls.beat();
                                              }] retain];
    [[NSRunLoop mainRunLoop] addTimer:native->beats forMode:NSRunLoopCommonModes];
}

Watch::~Watch()
{
    *native->alive = false;

    for (NSTimer* timer : {native->timer, native->beats})
    {
        [timer invalidate];
        [timer release];
    }

    for (id observer : native->observers)
    {
        [[NSNotificationCenter defaultCenter] removeObserver:observer];
        [observer release];
    }

    [NSEvent removeMonitor:native->monitor];
    [native->monitor release];
}

void Watch::post(std::function<void()> function)
{
    std::weak_ptr<bool> living = native->alive;
    auto* kept = new std::function<void()>(std::move(function));
    dispatch_async(dispatch_get_main_queue(), ^{
      if (const auto still = living.lock(); still && *still)
          (*kept)();
      delete kept;
    });
}

bool inFront()
{
    return NSApp.active;
}

// Menus, tooltips and the like lie above floating windows, and are never stepped back for.
bool otherShowing(const std::function<bool(void* window)>& known)
{
    for (NSWindow* window in NSApp.windows)
        if (window.visible && window.level <= NSFloatingWindowLevel && !known(window))
            return true;

    return false;
}

bool hasKeyboard(void* window)
{
    return static_cast<NSWindow*>(window).keyWindow;
}

bool typingIn(void* window)
{
    NSResponder* responder = static_cast<NSWindow*>(window).firstResponder;

    if ([responder isKindOfClass:[NSText class]])
        return true;

    if ([responder conformsToProtocol:@protocol(NSTextInputClient)])
    {
        auto* client = static_cast<id<NSTextInputClient>>(responder);
        return client.hasMarkedText || client.selectedRange.location != NSNotFound;
    }

    return false;
}

}
