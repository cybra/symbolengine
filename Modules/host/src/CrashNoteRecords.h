#pragma once

#include "symbolengine/host/PluginCrashNote.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>

// What the crash note holds, laid out in the file the system writes from memory, and what each system does with it.
namespace symbolengine::crashNote
{

constexpr char magic[4] = {'S', 'E', 'C', 'N'};
constexpr std::uint32_t version = 1, running = 1, ended = 2;
constexpr int slotCount = 128, threadCount = 16;
constexpr std::size_t crashRoom = 32 * 1024;

/** A plugin made, as its description gives it. */
struct Slot
{
    std::uint32_t used;
    char format[16];
    char id[96];
    char name[160];
    char maker[160];
    char location[1024];
};

/** A thread that calls plugins: the plugin it is inside a call into, -1 for none, and which call. */
struct Thread
{
    std::int32_t slot;
    std::uint32_t audio;
    std::uint32_t crashed;
    char call[52];
};

struct Layout
{
    char magic[4];
    std::uint32_t version;
    std::uint32_t state;
    std::uint32_t crashLength;
    char context[1024];
    Slot slots[slotCount];
    Thread threads[threadCount];
    char crash[crashRoom];
};

/** While it lives, the calling thread's record says it is inside this call into the plugin in the slot. */
class InCall
{
public:
    InCall(PluginCrashNote* note, int slot, const char* call, bool audioThread) noexcept;
    ~InCall();

    InCall(const InCall&) = delete;
    InCall& operator=(const InCall&) = delete;

private:
    Thread* record = nullptr;
    std::int32_t previousSlot = -1;
    std::uint32_t previousAudio = 0;
    char previousCall[sizeof(Thread::call)] = {};
};

/** Each system's file in memory, and its crash handler. */
namespace native
{

/** The file made at the size and kept in memory, the system writing to it; null where it can't be. */
Layout* map(const std::filesystem::path& file, void*& handle);
void unmap(Layout* layout, void* handle);

/** A handler for the process's crashes, which adds the crash, a backtrace and the crashing thread to the layout, then
    lets the crash go on as it would have. */
void catchCrashes(Layout* layout, void* handle);
void stopCatching();

}

/** The calling thread's record in the layout, which it claimed with its first call into a plugin, or -1; for the crash
    handler, so reading only what the thread set before. */
int threadRecordIn(const Layout* layout) noexcept;

}
