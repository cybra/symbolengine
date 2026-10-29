#include "ScanWorkerCrash.h"

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <execinfo.h>
#include <fcntl.h>
#include <unistd.h>

namespace symbolengine::scanWorkerCrash
{

namespace
{

int noteFile = -1;

struct Named
{
    int signal;
    const char* name;
};

constexpr Named crashes[] = {{SIGSEGV, "SIGSEGV"}, {SIGBUS, "SIGBUS"},   {SIGILL, "SIGILL"},
                             {SIGFPE, "SIGFPE"},   {SIGABRT, "SIGABRT"}, {SIGTRAP, "SIGTRAP"}};

void put(const char* text)
{
    [[maybe_unused]] const auto written = write(noteFile, text, std::strlen(text));
}

// A number in decimal, with nothing that allocates.
void putNumber(int value)
{
    char digits[16];
    int at = 15;
    digits[at] = '\0';
    do
        digits[--at] = static_cast<char>('0' + value % 10);
    while ((value /= 10) > 0 && at > 0);
    put(digits + at);
}

// Only calls safe in a signal handler, backtrace aside, which was made ready at the start; then the crash ends the
// process as it would have, the handler having reset itself.
void handle(int signal, siginfo_t*, void*)
{
    if (noteFile >= 0)
    {
        lseek(noteFile, 0, SEEK_END);
        put("\n");
        for (const auto& crash : crashes)
            if (crash.signal == signal)
                put(crash.name);
        put(" (signal ");
        putNumber(signal);
        put(")\n");
        void* frames[64];
        backtrace_symbols_fd(frames, backtrace(frames, 64), noteFile);
    }
    raise(signal);
}

}

void begin(const std::filesystem::path& note)
{
    noteFile = open(note.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (noteFile < 0)
        return;

    // backtrace loads what it needs the first time, which a crash mustn't have to do.
    void* frames[1];
    backtrace(frames, 1);

    // A stack of its own for the main thread's handler, so a stack overflow can still be told.
    constexpr std::size_t size = 256 * 1024;
    stack_t stack{};
    stack.ss_sp = std::malloc(size);
    stack.ss_size = size;
    sigaltstack(&stack, nullptr);

    struct sigaction action{};
    action.sa_sigaction = handle;
    action.sa_flags = static_cast<int>(SA_SIGINFO | SA_ONSTACK | SA_RESETHAND);
    sigemptyset(&action.sa_mask);
    for (const auto& crash : crashes)
        sigaction(crash.signal, &action, nullptr);
}

void note(std::string_view text)
{
    if (noteFile < 0)
        return;
    [[maybe_unused]] const auto written = pwrite(noteFile, text.data(), text.size(), 0);
    [[maybe_unused]] const auto cut = ftruncate(noteFile, static_cast<off_t>(text.size()));
}

}
