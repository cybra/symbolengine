#pragma once

/** The program's main thread, readied for plugins before any is loaded. */
namespace symbolengine::mainThread
{

/** On Windows, OLE, which a plugin's window needs to take dropped files and which must come before any other use of COM
    on the thread; elsewhere nothing. On the main thread. */
void prepare();

}
