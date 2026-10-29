#include "ScanWorkerLoop.h"

#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>

#include <dispatch/dispatch.h>
#include <memory>
#include <pthread.h>
#include <unistd.h>

namespace symbolengine::scanWorkerLoop
{

void begin()
{
    @autoreleasepool
    {
        // Before the application runs, so no Dock icon ever appears for a worker.
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    }
}

void post(std::function<void()> function)
{
    // The main queue runs in every mode of the main run loop, a plugin's own modal panels among them.
    dispatch_async_f(dispatch_get_main_queue(), new std::function<void()>(std::move(function)), [](void* context) {
        const std::unique_ptr<std::function<void()>> task(static_cast<std::function<void()>*>(context));
        (*task)();
    });
}

bool onMainThread()
{
    return pthread_main_np() != 0;
}

void run()
{
    for (;;)
        [NSApp run];
}

bool showsWindow()
{
    // Core Graphics' list of windows on screen, which, unlike AppKit, any thread may read.
    bool shows = false;
    const auto windows = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements,
                                                    kCGNullWindowID);
    if (windows == nullptr)
        return false;
    // Normal, floating and modal windows, as a plugin's question may be any of them, and nothing at the Dock's level
    // or above.
    const auto self = static_cast<long long>(getpid());
    const auto dock = CGWindowLevelForKey(kCGDockWindowLevelKey);
    for (CFIndex i = 0; i < CFArrayGetCount(windows) && !shows; ++i)
    {
        const auto window = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(windows, i));
        long long owner = 0;
        int layer = -1;
        double alpha = 0;
        const auto ownerValue = static_cast<CFNumberRef>(CFDictionaryGetValue(window, kCGWindowOwnerPID));
        const auto layerValue = static_cast<CFNumberRef>(CFDictionaryGetValue(window, kCGWindowLayer));
        const auto alphaValue = static_cast<CFNumberRef>(CFDictionaryGetValue(window, kCGWindowAlpha));
        if (ownerValue != nullptr && layerValue != nullptr && alphaValue != nullptr &&
            CFNumberGetValue(ownerValue, kCFNumberLongLongType, &owner) &&
            CFNumberGetValue(layerValue, kCFNumberIntType, &layer) && CFNumberGetValue(alphaValue, kCFNumberDoubleType, &alpha))
            shows = owner == self && layer >= 0 && layer < dock && alpha > 0;
    }
    CFRelease(windows);
    return shows;
}

void comeForward()
{
    // Puts its windows above every other application's without taking the keyboard, which only the program can hand
    // it (letForward). Given to the main run loop itself, which runs it inside the plugin's own loop as it waits for an
    // answer, where the main queue, running one task at a time, would hold it until the plugin returned.
    CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopCommonModes, ^{
        for (NSWindow* window in NSApp.windows)
            if (window.visible)
                [window orderFrontRegardless];
    });
    CFRunLoopWakeUp(CFRunLoopGetMain());
}

void letForward(std::int64_t processId)
{
    // Only the program in front hands the front over, so a program working in the background, or a test, never takes
    // the keyboard from whoever is using the computer.
    if (NSApp == nil || !NSApp.active)
        return;
    NSRunningApplication* worker =
        [NSRunningApplication runningApplicationWithProcessIdentifier:static_cast<pid_t>(processId)];
    if (worker == nil)
        return;
    if (@available(macOS 14, *))
    {
        [NSApp yieldActivationToApplication:worker];
        [worker activateFromApplication:NSRunningApplication.currentApplication options:0];
    }
    else
        [worker activateWithOptions:NSApplicationActivateIgnoringOtherApps];
}

}
