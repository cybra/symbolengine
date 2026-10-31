#include "MainThread.h"

#if defined(__APPLE__)
    #include <CoreFoundation/CoreFoundation.h>
#endif

namespace symbolengine::mainThread
{

void prepare() {}

void turn()
{
#if defined(__APPLE__)
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.0, true);
#endif
}
}
