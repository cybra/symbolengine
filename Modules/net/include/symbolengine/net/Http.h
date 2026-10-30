#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/** Fetching from web servers, through the system's own client: its certificates, its proxies and its way of following
    redirects. NSURLSession on a Mac and WinHTTP on Windows; other systems say they can't yet. */
namespace symbolengine::http
{

struct Request
{
    /** An http or https address, already escaped. */
    std::string url{};

    /** Headers to send with it, such as a User-Agent. */
    std::vector<std::pair<std::string, std::string>> headers{};

    /** How long the connection may take, and then each wait for more of the answer. */
    std::chrono::milliseconds timeout{30000};
};

struct Answer
{
    /** The server's status, such as 200 or 404, with redirects followed; 0 where no server answered. */
    int status = 0;

    /** Why no answer came, or why it stopped short, in the system's words; empty for one that came whole or that the
        caller stopped. */
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
