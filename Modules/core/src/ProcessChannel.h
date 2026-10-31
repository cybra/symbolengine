#pragma once

#include "symbolengine/core/ChildProcess.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace symbolengine
{

/** The system's channel under a ProcessLink: a socket on a Mac and Linux, a pair of pipes on Windows, with, in a
    child, what it needs to watch the program that started it. Each system's source file defines its functions. */
struct ProcessLink::Native
{
#if defined(_WIN32)
    void* readHandle = nullptr;
    void* writeHandle = nullptr;
    void* parent = nullptr;         // a handle to the program that started this one, in a child
    void* readerThread = nullptr;   // the thread reading, so a read it waits in can be cancelled
    std::atomic<bool> reading{false};
    std::atomic<int> writers{0};
    std::atomic<unsigned long> writerThread{0};   // the thread writing, so a write it waits in can be cancelled
#else
    int socket = -1;
    std::int64_t parent = 0;   // the ID of the program that started this one, in a child
#endif
    std::atomic<bool> shut{false};

    ~Native();

    /** Writes all of it, or gives false. */
    bool write(const char* data, std::size_t size);

    /** Reads exactly this many bytes, or gives false at the channel's end, an error, or once shut. */
    bool read(char* data, std::size_t size);

    /** Called on the reading thread before its first read and after its last. */
    void beginReading();
    void endReading();

    /** Fails every call after and wakes a read waiting; from any thread. */
    void shutDown();

    /** Whether this is a child's end, watching the program that started it. */
    bool watchesParent() const;

    /** Waits until the program that started this one has ended, giving true, or stop is set, giving false. */
    bool waitForParent(const std::atomic<bool>& stop);
};

/** The environment variable naming a child's end of its link: its channel and the program that started it. */
inline constexpr const char* linkVariable = "SYMBOLENGINE_LINK";

}
