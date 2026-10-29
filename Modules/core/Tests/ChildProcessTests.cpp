#include <symbolengine/core/ChildProcess.h>
#include <symbolengine/core/Files.h>
#include <symbolengine/core/System.h>

#include "Scratch.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#else
    #include <cerrno>
    #include <csignal>
    #include <fcntl.h>
    #include <unistd.h>
#endif

using namespace symbolengine;
using namespace std::chrono_literals;

namespace
{

const std::filesystem::path helper = SYMBOLENGINE_CHILD_HELPER;

std::unique_ptr<ChildProcess> startHelper(std::vector<std::string> arguments, bool link = false,
                                          ChildProcess::Options options = {})
{
    options.arguments = std::move(arguments);
    options.link = link;
    std::error_code error;
    auto child = ChildProcess::start(helper, options, error);
    INFO(error.message());
    REQUIRE(child != nullptr);
    REQUIRE_FALSE(error);
    return child;
}

// What a link hears, kept for the test thread to wait on.
struct Heard
{
    std::mutex lock;
    std::condition_variable changed;
    std::vector<std::string> messages;
    bool lost = false;

    void listenTo(ProcessLink& link)
    {
        link.onMessage([this](std::string message) {
            const std::lock_guard held(lock);
            messages.push_back(std::move(message));
            changed.notify_all();
        });

        link.onLost([this] {
            const std::lock_guard held(lock);
            lost = true;
            changed.notify_all();
        });
    }

    bool waitForMessages(std::size_t count, std::chrono::milliseconds limit = 10s)
    {
        std::unique_lock held(lock);
        return changed.wait_for(held, limit, [&] { return messages.size() >= count; });
    }

    bool waitForLost(std::chrono::milliseconds limit)
    {
        std::unique_lock held(lock);
        return changed.wait_for(held, limit, [&] { return lost; });
    }

    // The messages up to one that says "end".
    std::vector<std::string> untilEnd(std::chrono::milliseconds limit = 10s)
    {
        std::unique_lock held(lock);
        changed.wait_for(held, limit, [&] { return !messages.empty() && messages.back() == "end"; });
        auto all = messages;

        if (!all.empty() && all.back() == "end")
            all.pop_back();

        return all;
    }
};

// Whether a process with this ID is gone, waited for up to the limit.
bool goneWithin(std::int64_t id, std::chrono::milliseconds limit)
{
    const auto until = std::chrono::steady_clock::now() + limit;

    for (;;)
    {
#if defined(_WIN32)
        const auto process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(id));

        if (process == nullptr)
            return true;

        const auto ended = WaitForSingleObject(process, 0) == WAIT_OBJECT_0;
        CloseHandle(process);

        if (ended)
            return true;
#else
        if (::kill(static_cast<pid_t>(id), 0) != 0 && errno == ESRCH)
            return true;
#endif

        if (std::chrono::steady_clock::now() > until)
            return false;

        std::this_thread::sleep_for(10ms);
    }
}

}

TEST_CASE("A child gets its arguments exactly as given")
{
    const std::vector<std::string> arguments = {"plain",
                                                "two words",
                                                "a \"quoted\" word",
                                                "back\\slash",
                                                "ends with a backslash\\",
                                                "\\\\server\\share",
                                                "",
                                                "naïve façade",
                                                "日本語のテキスト",
                                                "--name=a b",
                                                "tab\there",
                                                "a\\\"b",
                                                "\""};

    auto all = arguments;
    all.insert(all.begin(), "link-arguments");
    auto child = startHelper(all, true);
    Heard heard;
    heard.listenTo(*child->link());
    child->link()->start();

    CHECK(heard.untilEnd() == arguments);
}

TEST_CASE("A child's environment is this program's, with the changes asked for")
{
#if defined(_WIN32)
    _putenv_s("SYMBOLENGINE_TEST_INHERITED", "from the test");
    _putenv_s("SYMBOLENGINE_TEST_REMOVED", "should go");
#else
    setenv("SYMBOLENGINE_TEST_INHERITED", "from the test", 1);
    setenv("SYMBOLENGINE_TEST_REMOVED", "should go", 1);
#endif

    ChildProcess::Options options;
    options.environment = {{"SYMBOLENGINE_TEST_SET", "set for the child, été"},
                           {"SYMBOLENGINE_TEST_INHERITED", "replaced"}};
    options.removeFromEnvironment = {"SYMBOLENGINE_TEST_REMOVED"};
    auto child = startHelper({"link-environment", "SYMBOLENGINE_TEST_SET", "SYMBOLENGINE_TEST_INHERITED",
                              "SYMBOLENGINE_TEST_REMOVED", "PATH", "SYMBOLENGINE_LINK"},
                             true, options);
    Heard heard;
    heard.listenTo(*child->link());
    child->link()->start();
    const auto values = heard.untilEnd();

    REQUIRE(values.size() == 5);
    CHECK(values[0] == "set for the child, été");
    CHECK(values[1] == "replaced");
    CHECK(values[2] == "\x01unset");
    CHECK(values[3] != "\x01unset");
    CHECK(values[4] == "\x01unset");   // the link's own variable, taken out once the child opened its link
}

TEST_CASE("A child's output goes to the file asked for, or nowhere")
{
    ScratchFolder scratch;
    ChildProcess::Options options;
    options.outputFile = scratch.path / "folder made for it" / "output.txt";
    files::writeSafely(options.outputFile, "already here\n");
    auto child = startHelper({"print"}, false, options);
    const auto ending = child->wait(10s);

    REQUIRE(ending.has_value());
    CHECK(ending->kind == ProcessEnding::Kind::exited);
    CHECK(ending->code == 3);

    const auto output = files::read(options.outputFile);
    CHECK(output.starts_with("already here\n"));
    CHECK(output.find("to standard output") != std::string::npos);
    CHECK(output.find("to standard error") != std::string::npos);

    auto quiet = startHelper({"print"});
    const auto quietEnding = quiet->wait(10s);
    REQUIRE(quietEnding.has_value());
    CHECK(quietEnding->code == 3);
}

TEST_CASE("A child's exit code is said")
{
    for (const int code : {0, 1, 42, 255})
    {
        auto child = startHelper({"exit", std::to_string(code)});
        const auto ending = child->wait(10s);

        REQUIRE(ending.has_value());
        CHECK(ending->kind == ProcessEnding::Kind::exited);
        CHECK(ending->code == static_cast<std::uint32_t>(code));
        CHECK_FALSE(ending->killed);
        CHECK(child->ending().has_value());
    }
}

TEST_CASE("A child ended by a signal or an exception is said to be")
{
    auto child = startHelper({"end-by-signal"});
    const auto ending = child->wait(10s);

    REQUIRE(ending.has_value());
#if defined(_WIN32)
    CHECK(ending->kind == ProcessEnding::Kind::exception);
    CHECK(ending->code == 0xC0000005u);
#else
    CHECK(ending->kind == ProcessEnding::Kind::signal);
    CHECK(ending->code == static_cast<std::uint32_t>(SIGTERM));
#endif
    CHECK_FALSE(ending->killed);
}

TEST_CASE("endNow ends a process at once, running nothing more of it")
{
    ScratchFolder scratch;
    ChildProcess::Options options;
    options.outputFile = scratch.path / "output.txt";
    auto child = startHelper({"end-now"}, false, options);
    const auto ending = child->wait(10s);

    REQUIRE(ending.has_value());
    CHECK(ending->kind == ProcessEnding::Kind::exited);
    CHECK(ending->code == 9);
    CHECK(files::read(options.outputFile).find("destructor ran") == std::string::npos);
}

TEST_CASE("A child still running is said to be, and one that hangs is killed and confirmed gone")
{
    auto child = startHelper({"hang"});
    const auto id = child->id();

    CHECK(id > 0);
    CHECK_FALSE(child->wait(50ms).has_value());
    CHECK_FALSE(child->ending().has_value());

    CHECK(child->kill(2s));
    const auto ending = child->ending();
    REQUIRE(ending.has_value());
    CHECK(ending->killed);
    CHECK(goneWithin(id, 2s));
}

TEST_CASE("A child's memory in use is read while it runs, and none once it has ended")
{
    auto child = startHelper({"allocate", "300"});
    std::int64_t most = 0;
    const auto until = std::chrono::steady_clock::now() + 10s;
    while (most < 280LL * 1024 * 1024 && std::chrono::steady_clock::now() < until)
    {
        most = std::max(most, child->memoryInUse());
        std::this_thread::sleep_for(20ms);
    }
    CHECK(most >= 280LL * 1024 * 1024);
    CHECK(most < 400LL * 1024 * 1024);

    REQUIRE(child->kill());
    CHECK(child->memoryInUse() == 0);
}

TEST_CASE("Killing a child that already ended leaves how it ended")
{
    auto child = startHelper({"exit", "5"});
    REQUIRE(child->wait(10s).has_value());

    CHECK(child->kill());
    const auto ending = child->ending();
    REQUIRE(ending.has_value());
    CHECK(ending->kind == ProcessEnding::Kind::exited);
    CHECK(ending->code == 5);
    CHECK_FALSE(ending->killed);
}

TEST_CASE("A child still running when its object goes is ended with it")
{
    auto child = startHelper({"hang"});
    const auto id = child->id();
    child.reset();

    CHECK(goneWithin(id, 2s));
}

TEST_CASE("A program that can't be started says why")
{
    ScratchFolder scratch;
    std::error_code error;
    ChildProcess::Options options;

    CHECK(ChildProcess::start(scratch.path / "no such program", options, error) == nullptr);
    CHECK(error);

    error.clear();
    const auto notAProgram = scratch.path / "not a program";
    files::writeSafely(notAProgram, "just some text\n");
    CHECK(ChildProcess::start(notAProgram, options, error) == nullptr);
    CHECK(error);

    // And one that can be is started, so a start that always fails fails here too.
    error.clear();
    options.arguments = {"exit", "0"};
    CHECK(ChildProcess::start(helper, options, error) != nullptr);
    CHECK_FALSE(error);
}

TEST_CASE("Messages cross the link both ways, whole and in order")
{
    auto child = startHelper({"link-echo"}, true);
    Heard heard;
    heard.listenTo(*child->link());
    child->link()->start();

    std::vector<std::string> sent;

    for (const std::size_t size : {0u, 1u, 3u, 1000u, 65543u, 4u * 1024u * 1024u})
    {
        std::string message(size, '\0');

        for (std::size_t i = 0; i < size; ++i)
            message[i] = static_cast<char>((i * 7 + size) & 0xFF);

        sent.push_back(message);
        REQUIRE(child->link()->send(message));
    }

    REQUIRE(heard.waitForMessages(sent.size()));
    {
        const std::lock_guard held(heard.lock);
        CHECK(heard.messages == sent);
        heard.messages.clear();
    }

    // Four threads sending at once: every message arrives whole, each thread's in its order.
    constexpr int threads = 4, each = 200;
    std::vector<std::thread> senders;

    for (int t = 0; t < threads; ++t)
        senders.emplace_back([&, t] {
            for (int n = 0; n < each; ++n)
                child->link()->send(std::to_string(t) + ":" + std::to_string(n) + ":" +
                                    std::string(static_cast<std::size_t>(n * 37 % 5000), 'x'));
        });

    for (auto& sender : senders)
        sender.join();

    REQUIRE(heard.waitForMessages(threads * each));
    std::map<int, int> next;
    const std::lock_guard held(heard.lock);

    for (const auto& message : heard.messages)
    {
        const auto first = message.find(':'), second = message.find(':', first + 1);
        REQUIRE(second != std::string::npos);
        const auto t = std::stoi(message.substr(0, first)),
                   n = std::stoi(message.substr(first + 1, second - first - 1));
        CHECK(n == next[t]);
        next[t] = n + 1;
        CHECK(message.size() - second - 1 == static_cast<std::size_t>(n * 37 % 5000));
    }
}

TEST_CASE("A message longer than a link takes is refused, and the link carries on")
{
    auto child = startHelper({"link-echo"}, true);
    Heard heard;
    heard.listenTo(*child->link());
    child->link()->start();

    CHECK_FALSE(child->link()->send(std::string(ProcessLink::maximumMessage + 1, 'x')));
    CHECK(child->link()->send("still here"));
    REQUIRE(heard.waitForMessages(1));
    CHECK_FALSE(child->link()->isLost());
}

TEST_CASE("The link is lost as soon as the child ends")
{
    SECTION("a child that never opens its link")
    {
        auto child = startHelper({"no-link-exit"}, true);
        Heard heard;
        heard.listenTo(*child->link());
        child->link()->start();

        CHECK(heard.waitForLost(2s));
        CHECK(child->link()->isLost());
        CHECK_FALSE(child->link()->send("anyone there?"));
    }

    SECTION("a child killed while linked")
    {
        auto child = startHelper({"link-echo"}, true);
        Heard heard;
        heard.listenTo(*child->link());
        child->link()->start();
        REQUIRE(child->link()->send("hello"));
        REQUIRE(heard.waitForMessages(1));

        REQUIRE(child->kill());
        CHECK(heard.waitForLost(2s));
    }
}

TEST_CASE("A child whose link is closed ends by itself")
{
    auto child = startHelper({"link-echo"}, true);
    Heard heard;
    heard.listenTo(*child->link());
    child->link()->start();
    REQUIRE(child->link()->send("hello"));
    REQUIRE(heard.waitForMessages(1));

    child->link()->close();
    const auto ending = child->wait(2s);

    REQUIRE(ending.has_value());
    CHECK(ending->kind == ProcessEnding::Kind::exited);
    CHECK(ending->code == 7);
    CHECK_FALSE(heard.lost);   // closing it here is never "lost"
}

TEST_CASE("A child ends when the program that started it is killed")
{
    auto middle = startHelper({"middle", helper.string()}, true);
    Heard heard;
    heard.listenTo(*middle->link());
    middle->link()->start();
    REQUIRE(heard.waitForMessages(1));
    std::int64_t grandchild = 0;
    {
        const std::lock_guard held(heard.lock);
        grandchild = std::stoll(heard.messages.front());
    }

    CHECK_FALSE(goneWithin(grandchild, 200ms));
    REQUIRE(middle->kill());
    CHECK(goneWithin(grandchild, 2s));
}

TEST_CASE("A child whose program ended is told so, even when another process holds the program's end open")
{
    auto middle = startHelper({"middle-leaky", helper.string()}, true);
    Heard heard;
    heard.listenTo(*middle->link());
    middle->link()->start();
    REQUIRE(heard.waitForMessages(1));
    std::string said;
    {
        const std::lock_guard held(heard.lock);
        said = heard.messages.front();
    }
    const auto grandchild = std::stoll(said.substr(0, said.find(';')));
    [[maybe_unused]] const auto holder = std::stoll(said.substr(said.find(';') + 1));

    CHECK_FALSE(goneWithin(grandchild, 200ms));
    REQUIRE(middle->kill());
    CHECK(goneWithin(grandchild, 2s));

#if !defined(_WIN32)
    if (holder > 0)
        ::kill(static_cast<pid_t>(holder), SIGKILL);
#endif
}

TEST_CASE("A message that can't be read loses the link")
{
    // Lost at once, never waited on or made room for: the child sends nothing after it, and no heartbeat at all.
    const auto garbage = GENERATE(as<std::string>{}, "too-long", "bad-kind");
    auto child = startHelper({"link-garbage", garbage}, true);
    Heard heard;
    heard.listenTo(*child->link());
    child->link()->start();

    CHECK(heard.waitForLost(2s));
}

TEST_CASE("A link whose other end keeps beating is never lost", "[timing]")
{
    auto child = startHelper({"link-echo", "50", "5"}, true);
    Heard heard;
    heard.listenTo(*child->link());
    child->link()->start({50ms, 5});
    REQUIRE(child->link()->send("hello"));
    REQUIRE(heard.waitForMessages(1));

    CHECK_FALSE(heard.waitForLost(2s));
}

#if !defined(_WIN32)

TEST_CASE("A link is lost when nothing is heard for its silent beats", "[timing]")
{
    auto child = startHelper({"link-echo", "50", "5"}, true);
    Heard heard;
    heard.listenTo(*child->link());
    child->link()->start({50ms, 5});
    REQUIRE(child->link()->send("hello"));
    REQUIRE(heard.waitForMessages(1));

    // A child stopped by the system sends nothing, beats included, but its channel stays open.
    REQUIRE(::kill(static_cast<pid_t>(child->id()), SIGSTOP) == 0);
    const auto stopped = std::chrono::steady_clock::now();

    CHECK(heard.waitForLost(3s));
    CHECK(std::chrono::steady_clock::now() - stopped >= 200ms);
    child->kill();
}

TEST_CASE("A child inherits no open file of this program's but its link")
{
    ScratchFolder scratch;
    const auto path = scratch.path / "left open.txt";
    files::writeSafely(path, "x");
    // Without close-on-exec, as other code may leave one, and above the descriptors a child is given, where the
    // child's own would hide it.
    const int opened = open(path.c_str(), O_RDONLY);
    REQUIRE(opened >= 0);
    const int leftOpen = fcntl(opened, F_DUPFD, 50);
    close(opened);
    REQUIRE(leftOpen >= 50);

    auto child = startHelper({"link-files"}, true);
    Heard heard;
    heard.listenTo(*child->link());
    child->link()->start();
    REQUIRE(heard.waitForMessages(1));
    close(leftOpen);

    // Standard input, output and error, and the link's own channel.
    const std::lock_guard held(heard.lock);
    CHECK(heard.messages.front() == "0,1,2,3");
}

TEST_CASE("A child starts with every signal at its default, whatever this program ignores")
{
    const auto childBefore = std::signal(SIGCHLD, SIG_IGN);
    const auto pipeBefore = std::signal(SIGPIPE, SIG_IGN);
    auto child = startHelper({"link-signals"}, true);
    std::signal(SIGCHLD, childBefore);
    std::signal(SIGPIPE, pipeBefore);

    Heard heard;
    heard.listenTo(*child->link());
    child->link()->start();
    REQUIRE(heard.waitForMessages(1));
    const std::lock_guard held(heard.lock);
    CHECK(heard.messages.front() == "default default ");
}

TEST_CASE("A program that ignores SIGCHLD still learns its child ended")
{
    const auto before = std::signal(SIGCHLD, SIG_IGN);
    auto child = startHelper({"exit", "5"});
    const auto ending = child->wait(5s);
    std::signal(SIGCHLD, before);

    REQUIRE(ending.has_value());
    CHECK((ending->kind == ProcessEnding::Kind::unknown || ending->code == 5));
}

#endif

TEST_CASE("A program started without a link has none to open")
{
#if defined(_WIN32)
    _putenv_s("SYMBOLENGINE_LINK", "");
#else
    unsetenv("SYMBOLENGINE_LINK");
#endif
    std::error_code error;
    CHECK(ProcessLink::toParent(error) == nullptr);
    CHECK(error);

#if defined(_WIN32)
    _putenv_s("SYMBOLENGINE_LINK", "not a channel");
#else
    setenv("SYMBOLENGINE_LINK", "not a channel", 1);
#endif
    error.clear();
    CHECK(ProcessLink::toParent(error) == nullptr);
    CHECK(error);

#if defined(_WIN32)
    _putenv_s("SYMBOLENGINE_LINK", "");
#else
    unsetenv("SYMBOLENGINE_LINK");
#endif
}
