#pragma once

#include <symbolengine/host/Plugin.h>

#include "UnsafeCallsLink.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace symbolengine::unsafeCalls
{

// What sees the calls, or nothing where it isn't in this process.
const Link* link();

// Makes one call of the kinds a program's watching is tried with, a memory one and a lock.
void tryCalls();

// What a plugin's calls on the audio thread have been found to be, kept by that thread without waiting: one place for
// each call and the way to it, as one thread at a time plays a plugin, read from any other.
class Record
{
public:
    static constexpr int places = 64;
    static constexpr int framesKept = 40;

    // Told on the audio thread that a place is new, without waiting for anything.
    using Told = void (*)(void* context);

    Record(const Link& link, std::filesystem::path plugin, Told told, void* context);

    Record(const Record&) = delete;
    Record& operator=(const Record&) = delete;

    void enter(const char* step, std::int64_t block) noexcept;
    void leave() noexcept;

    std::vector<PluginUnsafeCall> calls() const;
    std::int64_t notListed() const { return unlisted.load(std::memory_order_relaxed); }

    static void saw(void* watched, const char* function, int kind, void* returnAddress);

private:
    struct Place
    {
        // Set last, once the rest of what doesn't change is, so a reader that sees it can read them.
        std::atomic<std::uint64_t> key{0};
        const char* function = nullptr;
        const char* step = nullptr;
        int kind = 0;
        int order = 0;
        int frameCount = 0;
        std::array<void*, framesKept> frames{};
        std::int64_t firstBlock = 0;
        std::atomic<std::int64_t> times{0}, blocks{0}, lastBlock{0};
    };

    void found(const char* function, int kind, void* returnAddress) noexcept;

    const Link& link;
    const std::filesystem::path plugin;
    const Told told;
    void* const context;

    // The audio thread's, as it enters a call.
    const char* step = nullptr;
    std::int64_t block = 0;
    int nextOrder = 0;

    std::array<Place, places> kept;
    std::atomic<std::int64_t> unlisted{0};
};

// Watches the thread for the length of a call into a plugin, for a plugin whose host watches.
class Watching
{
public:
    Watching(Record* watched, const char* step, std::int64_t block) noexcept : record(watched)
    {
        if (record != nullptr)
            record->enter(step, block);
    }

    ~Watching()
    {
        if (record != nullptr)
            record->leave();
    }

    Watching(const Watching&) = delete;
    Watching& operator=(const Watching&) = delete;

private:
    Record* const record;
};

// This system's part: the way to where a thread is, innermost first; a frame in words; and whether a module is the
// system's own.
int captureFrames(void** frames, int most) noexcept;

struct Frame
{
    std::filesystem::path module;
    std::string text;
};

Frame describeFrame(void* frame);
bool isSystemModule(const std::filesystem::path& module);
std::filesystem::path programModule();

}
