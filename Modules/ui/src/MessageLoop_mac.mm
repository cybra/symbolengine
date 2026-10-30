#include <symbolengine/ui/MessageLoop.h>

#include <CoreFoundation/CoreFoundation.h>

#include <memory>
#include <thread>

namespace symbolengine::ui::messageLoop
{

// Through the run loop itself, not the main dispatch queue callAsync goes through, whose calls wait for the one under
// way.
void callSoon(std::function<void()> function)
{
    auto held = std::make_shared<std::function<void()>>(std::move(function));
    const auto main = CFRunLoopGetMain();
    CFRunLoopPerformBlock(main, kCFRunLoopCommonModes, ^{
      (*held)();
    });
    CFRunLoopWakeUp(main);
}

bool runUntil(const std::function<bool()>& condition, std::chrono::milliseconds limit)
{
    const auto until = std::chrono::steady_clock::now() + limit;
    while (!condition())
    {
        if (std::chrono::steady_clock::now() >= until)
            return false;

        // A run loop with nothing to wait on says so at once.
        if (CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, true) == kCFRunLoopRunFinished)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

}
