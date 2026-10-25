#pragma once

#include <cstdint>
#include <functional>

/** The message loop a program's windows, web views and menus run on. One thread runs it, the message thread, and
    everything in the ui module is made and used there; other threads reach it through callAsync. */
namespace symbolengine::ui::messageLoop
{

/** Makes this thread the message thread before run(), so that callAsync, callAfter and isMessageThread work while the
    program sets up its windows. run() does the same on its own. */
void initialize();

/** Runs the loop on this thread, which becomes the message thread, until stop() is called. */
void run();

/** Stops the loop, so run() returns once what is already queued has run. From any thread. */
void stop();

/** Whether this is the message thread. */
bool isMessageThread();

/** Calls the function on the message thread, after everything queued before it. From any thread. */
void callAsync(std::function<void()> function);

/** Calls the function on the message thread once at least the milliseconds have passed. From the message thread. */
void callAfter(std::uint32_t milliseconds, std::function<void()> function);

}
