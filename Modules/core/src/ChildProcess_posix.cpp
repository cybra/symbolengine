#include "ProcessChannel.h"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <spawn.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#if defined(__APPLE__)
    #include <crt_externs.h>
    #include <libproc.h>
    #include <sys/event.h>
    #include <sys/resource.h>
#else
extern char** environ;
#endif

namespace symbolengine
{

namespace
{

char** currentEnvironment()
{
#if defined(__APPLE__)
    return *_NSGetEnviron();
#else
    return environ;
#endif
}

std::error_code lastError()
{
    return {errno, std::generic_category()};
}

// A descriptor moved above the standard ones and the link's, closed in any program this one starts.
int movedUp(int descriptor)
{
    const int moved = fcntl(descriptor, F_DUPFD_CLOEXEC, 10);
    ::close(descriptor);
    return moved;
}

void keepFromChildren(int descriptor)
{
    fcntl(descriptor, F_SETFD, fcntl(descriptor, F_GETFD) | FD_CLOEXEC);
#if defined(__APPLE__)
    // Writing to a link whose other end has gone fails instead of raising SIGPIPE, which would end this program.
    const int on = 1;
    setsockopt(descriptor, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#endif
}

std::string_view nameOf(std::string_view entry)
{
    return entry.substr(0, entry.find('='));
}

constexpr int linkDescriptor = 3;

}

ProcessLink::Native::~Native()
{
    if (socket >= 0)
        ::close(socket);
}

bool ProcessLink::Native::write(const char* data, std::size_t size)
{
#if defined(__APPLE__)
    constexpr int flags = 0;
#else
    constexpr int flags = MSG_NOSIGNAL;
#endif

    while (size > 0)
    {
        if (shut)
            return false;

        const auto written = ::send(socket, data, size, flags);

        if (written < 0 && errno == EINTR)
            continue;

        if (written <= 0)
            return false;

        data += written;
        size -= static_cast<std::size_t>(written);
    }

    return true;
}

bool ProcessLink::Native::read(char* data, std::size_t size)
{
    while (size > 0)
    {
        if (shut)
            return false;

        const auto got = ::recv(socket, data, size, 0);

        if (got < 0 && errno == EINTR)
            continue;

        if (got <= 0)
            return false;

        data += got;
        size -= static_cast<std::size_t>(got);
    }

    return true;
}

void ProcessLink::Native::beginReading() {}

void ProcessLink::Native::endReading() {}

void ProcessLink::Native::shutDown()
{
    // Shutting the socket down wakes a read or write waiting on it, on every thread.
    if (!shut.exchange(true) && socket >= 0)
        ::shutdown(socket, SHUT_RDWR);
}

bool ProcessLink::Native::watchesParent() const
{
    return parent > 0;
}

bool ProcessLink::Native::waitForParent(const std::atomic<bool>& stop)
{
    const auto parentId = static_cast<pid_t>(parent);

#if defined(__APPLE__)
    const int queue = kqueue();

    if (queue >= 0)
    {
        struct kevent change;
        EV_SET(&change, parentId, EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0, nullptr);

        if (kevent(queue, &change, 1, nullptr, 0, nullptr) < 0)
        {
            ::close(queue);
            return errno == ESRCH;
        }

        while (!stop)
        {
            struct kevent event;
            const timespec wait{0, 200'000'000};
            const int got = kevent(queue, nullptr, 0, &event, 1, &wait);

            if (got > 0 || getppid() != parentId)
            {
                ::close(queue);
                return true;
            }
        }

        ::close(queue);
        return false;
    }
#endif

    // Once the program that started this one ends, this one is given another parent.
    while (!stop)
    {
        if (getppid() != parentId)
            return true;

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    return false;
}

std::unique_ptr<ProcessLink> ProcessLink::toParent(std::error_code& error)
{
    const char* value = std::getenv(linkVariable);

    if (value == nullptr || *value == '\0')
    {
        error = std::make_error_code(std::errc::no_such_file_or_directory);
        return nullptr;
    }

    char* rest = nullptr;
    const long descriptor = std::strtol(value, &rest, 10);
    const long long parentId = rest != nullptr && *rest == ';' ? std::strtoll(rest + 1, nullptr, 10) : 0;
    int type = 0;
    socklen_t typeSize = sizeof type;

    if (rest == value || descriptor < 0 || descriptor > 1 << 20 || parentId <= 0 ||
        getsockopt(static_cast<int>(descriptor), SOL_SOCKET, SO_TYPE, &type, &typeSize) != 0 || type != SOCK_STREAM)
    {
        error = std::make_error_code(std::errc::invalid_argument);
        return nullptr;
    }

    // Kept from any program this one starts, whose holding the channel open would hide this one's end.
    unsetenv(linkVariable);
    auto native = std::make_unique<Native>();
    native->socket = static_cast<int>(descriptor);
    native->parent = parentId;
    keepFromChildren(native->socket);
    error.clear();
    return std::make_unique<ProcessLink>(std::move(native));
}

struct ChildProcess::Native
{
    pid_t id = -1;
    bool killing = false;
};

ChildProcess::ChildProcess(std::unique_ptr<Native> made) : native(std::move(made)) {}

ChildProcess::~ChildProcess()
{
    if (theLink != nullptr)
        theLink->close();

    kill();
}

std::unique_ptr<ChildProcess> ChildProcess::start(const std::filesystem::path& program, const Options& options,
                                                  std::error_code& error)
{
    error.clear();
    int channel[2] = {-1, -1};
    int output = -1;

    const auto cleanUp = [&] {
        for (const int descriptor : {channel[0], channel[1], output})
            if (descriptor >= 0)
                ::close(descriptor);
    };

    if (options.link)
    {
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, channel) != 0)
        {
            error = lastError();
            return nullptr;
        }

        channel[0] = movedUp(channel[0]);
        channel[1] = movedUp(channel[1]);

        if (channel[0] < 0 || channel[1] < 0)
        {
            error = lastError();
            cleanUp();
            return nullptr;
        }

        keepFromChildren(channel[0]);
    }

    if (!options.outputFile.empty())
    {
        if (options.outputFile.has_parent_path())
            std::filesystem::create_directories(options.outputFile.parent_path(), error);

        output = error ? -1 : ::open(options.outputFile.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);

        if (output < 0 || (output = movedUp(output)) < 0)
        {
            if (!error)
                error = lastError();

            cleanUp();
            return nullptr;
        }
    }

    // The environment: this program's, less what is taken out and the link this program may have been given, with
    // what is set.
    std::vector<std::string> environment;

    for (auto** entry = currentEnvironment(); entry != nullptr && *entry != nullptr; ++entry)
    {
        const std::string_view name = nameOf(*entry);
        bool keep = name != linkVariable;

        for (const auto& removed : options.removeFromEnvironment)
            keep = keep && name != removed;

        for (const auto& [set, value] : options.environment)
            keep = keep && name != set;

        if (keep)
            environment.emplace_back(*entry);
    }

    for (const auto& [name, value] : options.environment)
        environment.push_back(name + "=" + value);

    if (options.link)
        environment.push_back(std::string(linkVariable) + "=" + std::to_string(linkDescriptor) + ";" +
                              std::to_string(getpid()));

    std::vector<char*> environmentPointers;

    for (auto& entry : environment)
        environmentPointers.push_back(entry.data());

    environmentPointers.push_back(nullptr);

    std::string programPath = program.string();
    std::vector<std::string> arguments{programPath};
    arguments.insert(arguments.end(), options.arguments.begin(), options.arguments.end());
    std::vector<char*> argumentPointers;

    for (auto& argument : arguments)
        argumentPointers.push_back(argument.data());

    argumentPointers.push_back(nullptr);

    // Standard input reads nothing; output and error go to the file or nowhere; the link's end is descriptor 3; and
    // nothing else of this program's is open in the child.
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);

    if (output >= 0)
    {
        posix_spawn_file_actions_adddup2(&actions, output, 1);
        posix_spawn_file_actions_adddup2(&actions, output, 2);
    }
    else
    {
        posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0);
        posix_spawn_file_actions_adddup2(&actions, 1, 2);
    }

    if (options.link)
        posix_spawn_file_actions_adddup2(&actions, channel[1], linkDescriptor);

#if !defined(__APPLE__) && defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34))
    posix_spawn_file_actions_addclosefrom_np(&actions, linkDescriptor + 1);
#endif

    // Every signal at its default and none blocked, whatever this program set, such as SIGCHLD ignored.
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    sigset_t all, none;
    sigfillset(&all);
    sigemptyset(&none);
    posix_spawnattr_setsigdefault(&attributes, &all);
    posix_spawnattr_setsigmask(&attributes, &none);
    short flags = POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK;
#if defined(__APPLE__)
    flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
    posix_spawnattr_setflags(&attributes, flags);

    pid_t id = -1;
    const int failed = posix_spawn(&id, programPath.c_str(), &actions, &attributes, argumentPointers.data(),
                                   environmentPointers.data());
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);

    if (channel[1] >= 0)
        ::close(channel[1]);

    channel[1] = -1;

    if (output >= 0)
        ::close(output);

    output = -1;

    if (failed != 0)
    {
        error = {failed, std::generic_category()};
        cleanUp();
        return nullptr;
    }

    auto made = std::make_unique<Native>();
    made->id = id;
    std::unique_ptr<ChildProcess> child(new ChildProcess(std::move(made)));

    if (options.link)
    {
        auto end = std::make_unique<ProcessLink::Native>();
        end->socket = channel[0];
        child->theLink = std::make_unique<ProcessLink>(std::move(end));
    }

    return child;
}

std::int64_t ChildProcess::id() const
{
    return native->id;
}

std::int64_t ChildProcess::memoryInUse() const
{
#if defined(__APPLE__)
    // The footprint Activity Monitor shows, which counts compressed memory and not shared libraries' pages.
    rusage_info_v2 usage{};
    if (proc_pid_rusage(native->id, RUSAGE_INFO_V2, reinterpret_cast<rusage_info_t*>(&usage)) != 0)
        return 0;
    return static_cast<std::int64_t>(usage.ri_phys_footprint);
#else
    // Its resident pages, the second number in statm; a process that has ended has none.
    const auto path = "/proc/" + std::to_string(native->id) + "/statm";
    FILE* statm = std::fopen(path.c_str(), "r");
    if (statm == nullptr)
        return 0;
    long long size = 0;
    long long resident = 0;
    const auto read = std::fscanf(statm, "%lld %lld", &size, &resident);
    std::fclose(statm);
    return read == 2 ? resident * sysconf(_SC_PAGESIZE) : 0;
#endif
}

std::optional<ProcessEnding> ChildProcess::ending()
{
    return wait(std::chrono::milliseconds(0));
}

std::optional<ProcessEnding> ChildProcess::wait(std::chrono::milliseconds limit)
{
    const auto until = std::chrono::steady_clock::now() + limit;
    auto pause = std::chrono::milliseconds(1);

    while (!ended.has_value())
    {
        int status = 0;
        const pid_t got = waitpid(native->id, &status, WNOHANG);

        if (got == native->id)
        {
            ProcessEnding how;

            if (WIFEXITED(status))
                how = {ProcessEnding::Kind::exited, static_cast<std::uint32_t>(WEXITSTATUS(status)), false};
            else if (WIFSIGNALED(status))
                how = {ProcessEnding::Kind::signal, static_cast<std::uint32_t>(WTERMSIG(status)),
                       native->killing && WTERMSIG(status) == SIGKILL};
            else
                how = {ProcessEnding::Kind::unknown, 0, native->killing};

            ended = how;
            break;
        }

        if (got < 0 && errno == EINTR)
            continue;

        // The system took the child back itself, as it does for a program that ignores SIGCHLD: it has ended, but
        // how is no longer known.
        if (got < 0)
        {
            ended = ProcessEnding{ProcessEnding::Kind::unknown, 0, native->killing};
            break;
        }

        if (std::chrono::steady_clock::now() >= until)
            break;

        std::this_thread::sleep_for(pause);
        pause = std::min(pause * 2, std::chrono::milliseconds(20));
    }

    return ended;
}

bool ChildProcess::kill(std::chrono::milliseconds limit)
{
    if (ending().has_value())
        return true;

    native->killing = true;
    ::kill(native->id, SIGKILL);
    return wait(limit).has_value();
}

ProcessLink* ChildProcess::link()
{
    return theLink.get();
}

}
