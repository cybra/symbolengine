#include "CrashNoteRecords.h"

#include <algorithm>
#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <execinfo.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace symbolengine::crashNote::native
{

namespace
{

struct Named
{
    int signal;
    const char* name;
};

constexpr Named crashes[] = {{SIGSEGV, "SIGSEGV"}, {SIGBUS, "SIGBUS"},   {SIGILL, "SIGILL"},
                             {SIGFPE, "SIGFPE"},   {SIGABRT, "SIGABRT"}, {SIGTRAP, "SIGTRAP"}};

constexpr std::size_t crashCount = sizeof crashes / sizeof crashes[0];

// What the handler needs, set before it is installed and read in a crash.
Layout* caught = nullptr;
int caughtFile = -1;
struct sigaction before[crashCount];

// Text added to the crash's room in the note, with nothing that allocates.
void add(const char* text)
{
    const auto length = std::strlen(text);
    const auto at = caught->crashLength;
    const auto room = crashRoom - std::min<std::size_t>(at, crashRoom);
    const auto taken = std::min(length, room);
    std::memcpy(caught->crash + at, text, taken);
    caught->crashLength = static_cast<std::uint32_t>(at + taken);
}

void addNumber(int value)
{
    char digits[16];
    int at = 15;
    digits[at] = '\0';
    do
        digits[--at] = static_cast<char>('0' + value % 10);
    while ((value /= 10) > 0 && at > 0);
    add(digits + at);
}

// Only what is safe in a signal handler, the backtrace aside, which was made ready as the handler was installed: the
// thread marked as the one the crash came on, the signal and a backtrace written to the note, and then the crash goes
// on to whatever handled it before, the system's own report among them.
void onCrash(int signal, siginfo_t*, void*)
{
    if (caught != nullptr)
    {
        if (const auto index = threadRecordIn(caught); index >= 0)
            caught->threads[index].crashed = 1;

        for (const auto& crash : crashes)
            if (crash.signal == signal)
                add(crash.name);
        add(" (signal ");
        addNumber(signal);
        add(")\n");

        // The frames written through the file, which the system keeps one with the note's memory.
        if (caughtFile >= 0 &&
            lseek(caughtFile, static_cast<off_t>(offsetof(Layout, crash) + caught->crashLength), SEEK_SET) >= 0)
        {
            void* frames[64];
            backtrace_symbols_fd(frames, backtrace(frames, 64), caughtFile);
            const auto end = lseek(caughtFile, 0, SEEK_CUR);
            if (end > static_cast<off_t>(offsetof(Layout, crash)))
                caught->crashLength = static_cast<std::uint32_t>(
                    std::min<std::size_t>(static_cast<std::size_t>(end) - offsetof(Layout, crash), crashRoom));
        }
    }

    for (std::size_t i = 0; i < crashCount; ++i)
        if (crashes[i].signal == signal)
            sigaction(signal, &before[i], nullptr);

    raise(signal);
}

}

Layout* map(const std::filesystem::path& file, void*& handle)
{
    const int opened = open(file.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);

    if (opened < 0)
        return nullptr;

    if (ftruncate(opened, static_cast<off_t>(sizeof(Layout))) != 0)
    {
        close(opened);
        return nullptr;
    }

    void* memory = mmap(nullptr, sizeof(Layout), PROT_READ | PROT_WRITE, MAP_SHARED, opened, 0);

    if (memory == MAP_FAILED)
    {
        close(opened);
        return nullptr;
    }

    handle = reinterpret_cast<void*>(static_cast<std::intptr_t>(opened));
    return static_cast<Layout*>(memory);
}

void unmap(Layout* layout, void* handle)
{
    munmap(layout, sizeof(Layout));
    close(static_cast<int>(reinterpret_cast<std::intptr_t>(handle)));
}

void catchCrashes(Layout* layout, void* handle)
{
    caught = layout;
    caughtFile = static_cast<int>(reinterpret_cast<std::intptr_t>(handle));

    // backtrace loads what it needs the first time, which a crash mustn't have to do.
    void* frames[1];
    backtrace(frames, 1);

    // A stack of its own for the handler on this thread, unless it has one, so a stack overflow can still be told.
    if (stack_t current{}; sigaltstack(nullptr, &current) == 0 && current.ss_sp == nullptr)
    {
        constexpr std::size_t size = 256 * 1024;
        stack_t stack{};
        stack.ss_sp = std::malloc(size);
        stack.ss_size = size;
        sigaltstack(&stack, nullptr);
    }

    struct sigaction action{};
    action.sa_sigaction = onCrash;
    action.sa_flags = static_cast<int>(SA_SIGINFO | SA_ONSTACK);
    sigemptyset(&action.sa_mask);
    for (std::size_t i = 0; i < crashCount; ++i)
        sigaction(crashes[i].signal, &action, &before[i]);
}

void stopCatching()
{
    for (std::size_t i = 0; i < crashCount; ++i)
        sigaction(crashes[i].signal, &before[i], nullptr);

    caught = nullptr;
    caughtFile = -1;
}

}
