// A small program that fetches what is at a web address and says its status and size, through the system's own client,
// with its certificates, its proxies and its way of following redirects.

#include <symbolengine/net/Http.h>

#include <cstddef>
#include <cstdio>
#include <string_view>

using namespace symbolengine;

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::printf("Give it a web address.\n");
        return 2;
    }

    http::Request request{argv[1]};
    request.headers = {{"User-Agent", "SymbolEngine Fetch"}};

    // Each piece comes on a thread of the system's while get waits, so nothing else touches the count until it returns.
    std::size_t size = 0;
    const auto answer = http::get(request, [&size](int, std::string_view piece) {
        size += piece.size();
        return true;
    });

    // A status of 0 means no server answered. A problem with a status means the answer stopped short.
    if (!answer.problem.empty())
    {
        std::printf("Status %d, %zu bytes before it stopped. %s\n", answer.status, size, answer.problem.c_str());
        return 1;
    }

    std::printf("Status %d, %zu bytes\n", answer.status, size);
    return 0;
}
