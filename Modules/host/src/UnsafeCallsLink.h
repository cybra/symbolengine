#pragma once

// What passes between the host and what sees a plugin's unsafe calls: on a Mac and Linux a library of its own, which
// knows nothing else of the host; on Windows part of the host.

namespace symbolengine::unsafeCalls
{

// As PluginUnsafeCall::Kind.
enum KindNumber
{
    memoryCall,
    lockCall,
    sleepCall,
    fileCall,
    threadCall,
    networkCall,
};

// Told on the thread that made a call of what was being watched on it, the function's name, its kind and the address
// the call returns to. No call it makes is seen.
using Saw = void (*)(void* watched, const char* function, int kind, void* returnAddress);

struct Link
{
    // Who to tell, once, before the first enter.
    void (*listen)(Saw saw);

    // What the calling thread's calls are now being watched for, until leave: only what is safe on an audio thread, and
    // nothing that overlaps on one thread.
    void (*enter)(void* watched);
    void (*leave)();

    // Takes in what has been loaded since it last did, where it has to be told; nothing elsewhere. The main thread's.
    void (*refresh)();
};

}
