#pragma once

#include <chrono>
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

/** Calls the function on the message thread soon, after those given to this before it, whatever the thread is doing
    there: as it runs the loop, a modal loop of its own or runUntil, or inside a call that came through callAsync, whose
    calls wait for each other. From any thread. */
void callSoon(std::function<void()> function);

/** On the message thread: turns the loop until the condition holds or the limit passes, for a call there that needs
    what only the loop can bring, as a plugin made through the system's registry. What was given to callSoon runs
    meanwhile, as do timers and the system's events, so what else the program does on this thread may run within it.
    Gives whether the condition held. */
bool runUntil(const std::function<bool()>& condition, std::chrono::milliseconds limit);

/** Calls the function on the message thread once at least the milliseconds have passed. From the message thread. */
void callAfter(std::uint32_t milliseconds, std::function<void()> function);

}
