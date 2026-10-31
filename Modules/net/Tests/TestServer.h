#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

/** A web server for one test, on the computer's own loopback address and a port the system picks, reachable from nothing
    else: each connection is read up to the end of its request, handed to the function given as what it sent and a way to
    send, and closed when the function returns. It stops with the object. */
class TestServer
{
public:
    /** Sends bytes to the client that asked, saying whether they went. */
    using Send = std::function<bool(const std::string&)>;

    explicit TestServer(std::function<void(const std::string& request, const Send& send)> answer);
    ~TestServer();

    TestServer(const TestServer&) = delete;
    TestServer& operator=(const TestServer&) = delete;

    /** The address of a path on it, as http://127.0.0.1:port/path. */
    std::string address(const std::string& path) const;

    /** How many connections it has taken. */
    int connections() const { return taken.load(); }

private:
    std::uintptr_t listener = 0;
    int port = 0;
    std::atomic<int> taken{0};
    std::atomic<bool> stopping{false};
    std::thread thread;
};
