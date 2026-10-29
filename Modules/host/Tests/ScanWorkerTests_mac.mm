#import <AppKit/AppKit.h>

#include <cstdint>

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
