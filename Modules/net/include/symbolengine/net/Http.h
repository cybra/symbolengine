#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/** Fetching from web servers, through the system's own client: its certificates, its proxies and its way of following
    redirects. NSURLSession on a Mac and WinHTTP on Windows; other systems say they can't yet. */
namespace symbolengine::http
{

/** Stops a fetch from another thread, as a program quitting does while a server says nothing, where the function given
    the answer isn't being called to give false. It has to outlast the fetch it is given to, and once stopped it stays
    stopped. */
class Stopper
{
public:
    Stopper() = default;
    Stopper(const Stopper&) = delete;
    Stopper& operator=(const Stopper&) = delete;

    /** From any thread, at any time and more than once: the fetch given this returns soon after with no problem, and one
        given it from now on asks for nothing. It doesn't wait for the fetch to return. */
    void stop();

    bool stopped() const noexcept;

private:
    friend class StoppingFetch;
    std::mutex lock;
    std::atomic<bool> asked{false};
    std::function<void()> ending;
};

struct Request
{
    /** An http or https address, already escaped. */
    std::string url{};

    /** Headers to send with it, such as a User-Agent. */
    std::vector<std::pair<std::string, std::string>> headers{};

    /** How long the connection may take, and then each wait for more of the answer. */
    std::chrono::milliseconds timeout{30000};

    /** What can stop it from another thread, if anything. */
    Stopper* stopper = nullptr;
};

struct Answer
{
    /** The server's status, such as 200 or 404, with redirects followed; 0 where no server answered. */
    int status = 0;

    /** Why no answer came, or why it stopped short, in the system's words; empty for one that came whole or that the
        caller stopped. An answer that stopped short may have had pieces held back that never reached the function. */
    std::string problem{};
};

/** Gets what is at the address, waiting until it has come. Each piece of the answer, whatever its status, goes to the
    function as it arrives, in order, on a thread of the system's while this waits; the function gives false to stop. */
Answer get(const Request& request, const std::function<bool(int status, std::string_view piece)>& received);

/** Whether this system can fetch at all. */
bool available();

/** One part of an address's path, every character but letters, digits and -._~ escaped, a slash among them. */
std::string escaped(std::string_view part);

/** The file:// address of a file or folder, given by its full path. */
std::string fileAddress(const std::filesystem::path& file);

/** The file a file:// address names; nothing for any other address. */
std::optional<std::filesystem::path> fileAt(std::string_view address);

}
