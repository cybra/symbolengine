#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace symbolengine
{

class ProcessLink;

/** How a process ended. */
struct ProcessEnding
{
    enum class Kind
    {
        exited,      ///< It returned from main or called exit; code is its exit code.
        signal,      ///< A signal ended it, on a Mac or Linux; code is the signal's number.
        exception,   ///< The system ended it for an exception, on Windows; code is the exception's, such as 0xC0000005.
        unknown,     ///< It ended, but the system no longer says how, as when this program ignores SIGCHLD.
    };

    Kind kind = Kind::exited;
    std::uint32_t code = 0;

    /** Whether it ended because ChildProcess::kill() ended it. */
    bool killed = false;
};

/** A program this one started, held for as long as it may run: its process, how it ended, and a link to it when
    asked for. The child gets exactly the arguments and environment given, its standard output and error go to a file
    or nowhere, and it inherits nothing else of this program's: no open files, sockets or handles, and every signal at
    its default. Nothing about it waits on this program's other threads.

    A child never outlives what started it. Destroying the object kills the child if it still runs and waits for it to
    end. On Windows every child is in a job that the system ends with this program, however this program ends, and a
    crash in a child never shows the system's error dialog. A child that opened its link to this program also ends
    when this program goes, through the link (ProcessLink). */
class ChildProcess
{
public:
    struct Options
    {
        /** The arguments after the program's own path, as UTF-8, each passed exactly as given. */
        std::vector<std::string> arguments;

        /** Variables set in the child's environment, over those it inherits from this program. */
        std::vector<std::pair<std::string, std::string>> environment;

        /** Variables taken out of the child's environment, such as a library path this program was started with. */
        std::vector<std::string> removeFromEnvironment;

        /** A file the child's standard output and error are added to, made if it doesn't exist; with none, they are
            thrown away. A file and never a pipe, so a child that writes a great deal never waits for this side. */
        std::filesystem::path outputFile;

        /** Whether to make a link to the child, which it opens with ProcessLink::toParent(). */
        bool link = false;
    };

    /** Starts the program, given by its full path, or gives nothing and sets the error. */
    static std::unique_ptr<ChildProcess> start(const std::filesystem::path& program, const Options& options,
                                               std::error_code& error);

    ~ChildProcess();

    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    /** The process's ID, as the system shows it. */
    std::int64_t id() const;

    /** How it ended, or nothing while it runs. */
    std::optional<ProcessEnding> ending();

    /** Waits up to the limit for it to end, giving how it ended, or nothing if it still runs. */
    std::optional<ProcessEnding> wait(std::chrono::milliseconds limit);

    /** Ends it at once, giving it no chance to run anything more (SIGKILL; TerminateProcess), and waits up to the limit
        for the system to say it has ended, giving whether it has. A child that already ended is left as it was. */
    bool kill(std::chrono::milliseconds limit = std::chrono::seconds(2));

    /** The link to it, if it was started with one; it is started with ProcessLink::start(). */
    ProcessLink* link();

    struct Native;

private:
    explicit ChildProcess(std::unique_ptr<Native>);

    std::unique_ptr<Native> native;
    std::unique_ptr<ProcessLink> theLink;
    std::optional<ProcessEnding> ended;
};

/** Messages both ways between a program and a child it started, over a channel of the system's that only those two
    hold, so they reach nothing else and nothing else can hold it open. Each message arrives whole and in order, any
    bytes up to maximumMessage long.

    The link says when the other end has gone, and says it at once from a thread of its own, never through the
    program's main thread, which in a child may be held by code it loaded: when the other end closes the link or its
    process ends, when a message can't be read, when nothing is heard for silentBeats of this side's own heartbeats
    once the other end has been heard from (counted in beats, not by the clock, so a computer asleep loses no beats), and, in a child, when the program that
    started it ends. A child that is to end with what started it calls system::endNow() from onLost. */
class ProcessLink
{
public:
    struct Timing
    {
        /** How often each side tells the other it is there. */
        std::chrono::milliseconds heartbeat{1000};

        /** How many of this side's heartbeats may pass with nothing heard from the other before the link is lost,
            counted from the first thing heard: a program that never says anything is for its caller's time limit. */
        int silentBeats = 5;
    };

    static constexpr std::size_t maximumMessage = 64 * 1024 * 1024;

    /** In a child started with a link: the link to the program that started it, or nothing if there is none or it
        can't be used, with the error set. Its channel is kept from any program this child starts in turn. */
    static std::unique_ptr<ProcessLink> toParent(std::error_code& error);

    /** Closes the link, as close() does. Never destroy it from inside its own callbacks. */
    ~ProcessLink();

    ProcessLink(const ProcessLink&) = delete;
    ProcessLink& operator=(const ProcessLink&) = delete;

    /** Called on the link's thread with each message, in order. The link reads nothing more while it runs, so it hands
        long work elsewhere. Set before start(). */
    void onMessage(std::function<void(std::string)> callback);

    /** Called once, on one of the link's threads, when the other end has gone. Never called for this side's own
        close(). Set before start(). */
    void onLost(std::function<void()> callback);

    /** Starts reading and sending heartbeats, every second with five silent beats, or as the timing says. */
    void start();
    void start(Timing timing);

    /** Sends a message, from any thread, giving false once the link is lost or closed, or for a message longer than
        maximumMessage. */
    bool send(std::string_view message);

    /** Whether the other end has gone. */
    bool isLost() const;

    /** Closes the link from this side; the other end finds it lost. */
    void close();

    struct Native;

    /** For ChildProcess: a link over a channel it made. */
    explicit ProcessLink(std::unique_ptr<Native>);

private:
    struct State;
    std::shared_ptr<State> state;
};

}
