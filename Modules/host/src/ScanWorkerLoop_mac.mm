#include "ScanWorkerLoop.h"

#import <AppKit/AppKit.h>

#include <dispatch/dispatch.h>
#include <memory>
#include <pthread.h>

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

}
