// A program the child process tests start, doing what its first argument names.

#include <symbolengine/core/ChildProcess.h>
#include <symbolengine/core/System.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
    #include <windows.h>

    // After windows.h, which it needs.
    #include <shellapi.h>
#else
    #include <csignal>
    #include <fcntl.h>
    #include <unistd.h>
#endif

using namespace symbolengine;

namespace
{

[[noreturn]] void hang()
{
    for (;;)
        std::this_thread::sleep_for(std::chrono::seconds(1));
}

// The link to the test, which ends this program with 7 once lost, as a scan worker ends.
std::unique_ptr<ProcessLink> linkToParent(ProcessLink::Timing timing = {})
{
    std::error_code error;
    auto link = ProcessLink::toParent(error);

    if (link == nullptr)
    {
        std::fprintf(stderr, "no link: %s\n", error.message().c_str());
        std::exit(90);
    }

    link->onLost([] { system::endNow(7); });
    link->start(timing);
    return link;
}

ProcessLink::Timing timingFrom(int argc, char** argv, int at)
{
    ProcessLink::Timing timing;

    if (argc > at + 1)
    {
        timing.heartbeat = std::chrono::milliseconds(std::atoi(argv[at]));
        timing.silentBeats = std::atoi(argv[at + 1]);
    }

    return timing;
}

struct SaysWhenDestroyed
{
    ~SaysWhenDestroyed() { std::puts("destructor ran"); }
};

SaysWhenDestroyed saysWhenDestroyed;

}

int main(int argc, [[maybe_unused]] char** narrowArguments)
{
    // The arguments as UTF-8: Windows gives main them in the system's code page, so they are read again as UTF-16.
    std::vector<std::string> arguments;

#if defined(_WIN32)
    int count = 0;
    if (auto** wide = CommandLineToArgvW(GetCommandLineW(), &count))
    {
        for (int i = 0; i < count; ++i)
        {
            const auto text = std::filesystem::path(wide[i]).u8string();
            arguments.emplace_back(reinterpret_cast<const char*>(text.data()), text.size());
        }

        LocalFree(wide);
    }
#else
    arguments.assign(narrowArguments, narrowArguments + argc);
#endif

    std::vector<char*> pointers;
    for (auto& argument : arguments)
        pointers.push_back(argument.data());
    argc = static_cast<int>(pointers.size());
    char** argv = pointers.data();

    const std::string mode = argc > 1 ? argv[1] : "";

    if (mode == "exit")
        return std::atoi(argv[2]);

    if (mode == "print")
    {
        std::cout << "to standard output" << std::endl;
        std::cerr << "to standard error" << std::endl;
        return 3;
    }

    if (mode == "end-now")
    {
        std::fflush(stdout);
        system::endNow(9);
    }

    if (mode == "end-by-signal")
    {
#if defined(_WIN32)
        volatile int* nowhere = nullptr;
        *nowhere = 1;
#else
        std::raise(SIGTERM);
#endif
        return 0;
    }

    // Writes a file a little after it starts, to show it was still running by then.
    if (mode == "write-later")
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        std::ofstream(argv[2]).put('\n');
        return 0;
    }

    if (mode == "hang")
        hang();

    // Holds the megabytes given, each page written so the system counts it, until ended.
    if (mode == "allocate")
    {
        static std::vector<char> held(static_cast<std::size_t>(std::atoi(argv[2])) * 1024 * 1024);
        for (std::size_t i = 0; i < held.size(); i += 4096)
            held[i] = static_cast<char>(i);
        std::puts("allocated");
        std::fflush(stdout);
        hang();
    }

    if (mode == "link-echo")
    {
        std::error_code error;
        auto link = ProcessLink::toParent(error);

        if (link == nullptr)
            return 90;

        auto* raw = link.get();
        link->onMessage([raw](std::string message) { raw->send(message); });
        link->onLost([] { system::endNow(7); });
        link->start(timingFrom(argc, argv, 2));
        hang();
    }

    if (mode == "link-arguments")
    {
        auto link = linkToParent();

        for (int i = 2; i < argc; ++i)
            link->send(argv[i]);

        link->send("end");
        hang();
    }

    if (mode == "link-environment")
    {
        auto link = linkToParent();

        for (int i = 2; i < argc; ++i)
        {
            const auto value = system::environmentVariable(argv[i]);
            link->send(value.has_value() ? *value : std::string("\x01unset"));
        }

        link->send("end");
        hang();
    }

    if (mode == "link-files")
    {
        // Counted before the link starts the threads and handles of its own.
        std::string open;

#if defined(_WIN32)
        open = "not counted";
#else
        for (int fd = 0; fd < 4096; ++fd)
            if (fcntl(fd, F_GETFD) != -1)
                open += (open.empty() ? "" : ",") + std::to_string(fd);
#endif

        auto link = linkToParent();
        link->send(open);
        hang();
    }

    if (mode == "link-garbage")
    {
        // A frame no link can read, written straight onto the channel: a message one byte longer than any may be, or
        // a kind of frame there is none of.
#if defined(_WIN32)
        char channel[128] = {};
        if (GetEnvironmentVariableA("SYMBOLENGINE_LINK", channel, sizeof channel) == 0 || argc < 3)
            return 90;
#else
        const char* channel = std::getenv("SYMBOLENGINE_LINK");
        if (channel == nullptr || argc < 3)
            return 90;
#endif

        const auto tooLong = static_cast<std::uint32_t>(ProcessLink::maximumMessage + 2);
        const bool badKind = std::string(argv[2]) == "bad-kind";
        const unsigned char frame[] = {static_cast<unsigned char>(badKind ? 1 : tooLong & 0xFF),
                                       static_cast<unsigned char>(badKind ? 0 : (tooLong >> 8) & 0xFF),
                                       static_cast<unsigned char>(badKind ? 0 : (tooLong >> 16) & 0xFF),
                                       static_cast<unsigned char>(badKind ? 0 : (tooLong >> 24) & 0xFF),
                                       static_cast<unsigned char>(badKind ? 7 : 0)};

#if defined(_WIN32)
        unsigned long long readHandle = 0, writeHandle = 0, parent = 0;
        sscanf_s(channel, "%llu;%llu;%llu", &readHandle, &writeHandle, &parent);
        DWORD written = 0;
        WriteFile(reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(writeHandle)), frame, sizeof frame, &written,
                  nullptr);
#else
        const int fd = std::atoi(channel);
        [[maybe_unused]] const auto written = write(fd, frame, sizeof frame);
#endif
        hang();
    }

    if (mode == "link-signals")
    {
        auto link = linkToParent();
        std::string said;

#if defined(_WIN32)
        said = "not counted";
#else
        for (const int number : {SIGCHLD, SIGPIPE})
        {
            struct sigaction action{};
            sigaction(number, nullptr, &action);
            said += action.sa_handler == SIG_DFL ? "default " : "changed ";
        }
#endif

        link->send(said);
        hang();
    }

    if (mode == "middle-leaky")
    {
        // As "middle", but a process of its own also holds its end of the grandchild's link, as a helper a plugin
        // starts can, so only the grandchild's watch on this program sees it end.
        auto link = linkToParent();
        ChildProcess::Options options;
        options.arguments = {"link-echo"};
        options.link = true;
        std::error_code error;
        auto grandchild = ChildProcess::start(argv[2], options, error);

        if (grandchild == nullptr)
            return 91;

        grandchild->link()->start();

#if defined(_WIN32)
        link->send(std::to_string(grandchild->id()) + ";0");
#else
        const pid_t holder = fork();

        if (holder == 0)
        {
            sleep(30);
            _exit(0);
        }

        link->send(std::to_string(grandchild->id()) + ";" + std::to_string(holder));
#endif
        hang();
    }

    if (mode == "no-link-exit")
        return 0;

#if defined(__linux__)
    // The numbers of the files it has open, each between commas, into the file named.
    if (mode == "no-link-files")
    {
        // What each number is open on, as the listing itself holds the lowest number free.
        std::string open;
        std::error_code ignored;
        for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd"))
            open += std::filesystem::read_symlink(entry.path(), ignored).string() + "\n";
        std::ofstream(argv[2]) << open;
        return 0;
    }
#endif

    if (mode == "middle")
    {
        // Starts a grandchild linked to this program, tells the test its ID, and waits to be killed.
        auto link = linkToParent();
        ChildProcess::Options options;
        options.arguments = {"link-echo"};
        options.link = true;
        std::error_code error;
        auto grandchild = ChildProcess::start(argv[2], options, error);

        if (grandchild == nullptr)
            return 91;

        grandchild->link()->start();
        link->send(std::to_string(grandchild->id()));
        hang();
    }

    std::fprintf(stderr, "unknown mode %s\n", mode.c_str());
    return 99;
}
