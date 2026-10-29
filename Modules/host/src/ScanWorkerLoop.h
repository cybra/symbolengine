#pragma once

#include <cstdint>
#include <functional>

// A scan worker's main thread and its run loop, its own on each system, so a worker needs nothing of the ui module and
// runs nothing of the program's: on a Mac the application's, as an accessory from its start, so it never shows in the
// Dock; on Windows a message loop whose tasks reach it through a window of its own, so a plugin's own modal loop runs
// them too, with OLE set up as plugins need; elsewhere a queue.
namespace symbolengine::scanWorkerLoop
{

/** Makes the calling thread the worker's main thread, before anything is posted to it. */
void begin();

/** Runs a function on the main thread after those posted before it, from any thread. */
void post(std::function<void()> function);

bool onMainThread();

/** Runs the loop on the main thread for as long as the process lives. */
[[noreturn]] void run();

/** Whether the worker shows a window of its own on screen, as a plugin's question does. From any thread. */
bool showsWindow();

/** Brings the worker and its windows to the front. From any thread. */
void comeForward();

/** In the program: lets the worker with this process ID come to the front, as only the program in front can. */
void letForward(std::int64_t processId);

}
