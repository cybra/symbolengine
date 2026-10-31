#include "TestServer.h"

#include <chrono>
#include <cstring>
#include <stdexcept>
#include <vector>

#if defined(_WIN32)
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
#else
    #include <arpa/inet.h>
    #include <netinet/in.h>
    #include <poll.h>
    #include <sys/socket.h>
    #include <unistd.h>
#endif

namespace
{

#if defined(_WIN32)
using Socket = SOCKET;
constexpr Socket noSocket = INVALID_SOCKET;

void closeSocket(Socket socket)
{
    closesocket(socket);
}

bool readable(Socket socket, int milliseconds)
{
    WSAPOLLFD waiting{socket, POLLIN, 0};
    return WSAPoll(&waiting, 1, milliseconds) > 0;
}

struct Sockets
{
    Sockets()
    {
        WSADATA data{};
        WSAStartup(MAKEWORD(2, 2), &data);
    }

    ~Sockets() { WSACleanup(); }
};
#else
using Socket = int;
constexpr Socket noSocket = -1;

void closeSocket(Socket socket)
{
    close(socket);
}

bool readable(Socket socket, int milliseconds)
{
    pollfd waiting{socket, POLLIN, 0};
    return poll(&waiting, 1, milliseconds) > 0;
}

struct Sockets
{
};
#endif

}

TestServer::TestServer(std::function<void(const std::string& request, const Send& send)> answer)
{
    static const Sockets sockets;
    static_cast<void>(sockets);

    const auto made = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    if (made == noSocket)
        throw std::runtime_error("The test server couldn't make its socket");

    sockaddr_in place{};
    place.sin_family = AF_INET;
    place.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    place.sin_port = 0;

    if (bind(made, reinterpret_cast<const sockaddr*>(&place), sizeof place) != 0 || listen(made, 8) != 0)
    {
        closeSocket(made);
        throw std::runtime_error("The test server couldn't listen on the loopback address");
    }

    socklen_t size = sizeof place;
    getsockname(made, reinterpret_cast<sockaddr*>(&place), &size);
    port = ntohs(place.sin_port);
    listener = static_cast<std::uintptr_t>(made);

    thread = std::thread([this, made, answer = std::move(answer)] {
        std::vector<std::thread> clients;

        while (!stopping.load())
        {
            if (!readable(made, 20))
                continue;

            const auto client = accept(made, nullptr, nullptr);

            if (client == noSocket)
                continue;

            ++taken;

#if defined(__APPLE__)
            const int noSignal = 1;
            setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &noSignal, sizeof noSignal);
#endif

            clients.emplace_back([this, client, &answer] {
                std::string request;
                char piece[4096];

                while (request.find("\r\n\r\n") == std::string::npos && !stopping.load())
                {
                    if (!readable(client, 20))
                        continue;

                    const auto got = recv(client, piece, sizeof piece, 0);

                    if (got <= 0)
                        break;

                    request.append(piece, static_cast<std::size_t>(got));
                }

                answer(request, [client](const std::string& bytes) {
                    std::size_t sent = 0;

                    while (sent < bytes.size())
                    {
#if defined(_WIN32)
                        const auto wrote =
                            ::send(client, bytes.data() + sent, static_cast<int>(bytes.size() - sent), 0);
#else
    #if defined(MSG_NOSIGNAL)
                        const auto wrote = ::send(client, bytes.data() + sent, bytes.size() - sent, MSG_NOSIGNAL);
    #else
                        const auto wrote = ::send(client, bytes.data() + sent, bytes.size() - sent, 0);
    #endif
#endif

                        if (wrote <= 0)
                            return false;

                        sent += static_cast<std::size_t>(wrote);
                    }

                    return true;
                });

                closeSocket(client);
            });
        }

        for (auto& client : clients)
            client.join();
    });
}

TestServer::~TestServer()
{
    stopping = true;
    thread.join();
    closeSocket(static_cast<Socket>(listener));
}

std::string TestServer::address(const std::string& path) const
{
    return "http://127.0.0.1:" + std::to_string(port) + path;
}
