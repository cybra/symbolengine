#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>

#include <libproc.h>

#include <cstdint>
#include <string>

// Whether the process is a regular application, with an icon in the Dock.
bool showsInDock(std::int64_t processId)
{
    @autoreleasepool
    {
        NSRunningApplication* application =
            [NSRunningApplication runningApplicationWithProcessIdentifier:static_cast<pid_t>(processId)];
        return application == nil || application.activationPolicy == NSApplicationActivationPolicyRegular;
    }
}

// The executable of the process whose window is in front of every other normal window on screen.
std::string frontmostExecutable()
{
    std::string path;
    const auto windows =
        CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID);
    for (CFIndex i = 0; windows != nullptr && i < CFArrayGetCount(windows) && path.empty(); ++i)
    {
        const auto window = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(windows, i));
        int layer = -1;
        int owner = 0;
        CFNumberGetValue(static_cast<CFNumberRef>(CFDictionaryGetValue(window, kCGWindowLayer)), kCFNumberIntType, &layer);
        CFNumberGetValue(static_cast<CFNumberRef>(CFDictionaryGetValue(window, kCGWindowOwnerPID)), kCFNumberIntType, &owner);
        char found[PROC_PIDPATHINFO_MAXSIZE] = {};
        if (layer == 0 && proc_pidpath(owner, found, sizeof found) > 0)
            path = found;
    }
    if (windows != nullptr)
        CFRelease(windows);
    return path;
}
