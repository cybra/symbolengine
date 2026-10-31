#include "symbolengine/net/Http.h"

#include "TestServer.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

using namespace symbolengine;
using namespace std::chrono_literals;

namespace
{

// An answer with a status and a body whose length it states.
std::string answerWith(const std::string& status, const std::string& body, const std::string& headers = {})
{
    return "HTTP/1.1 " + status + "\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n" +
           headers + "\r\n" + body;
}

struct Got
{
    http::Answer answer;
    std::string body;
    int pieces = 0;
    int lastStatus = 0;
};

Got fetch(http::Request request)
{
    Got got;
    got.answer = http::get(request, [&got](int status, std::string_view piece) {
        got.body += piece;
        ++got.pieces;
        got.lastStatus = status;
        return true;
    });
    return got;
}

}

TEST_CASE("A served file comes whole, with its status and the headers asked for reaching the server", "[served]")
{
    if (!http::available())
        SKIP("This system can't fetch yet");

    std::string asked;
    TestServer server([&asked](const std::string& request, const TestServer::Send& send) {
        asked = request;
        send(answerWith("200 OK", "the whole file"));
    });

    http::Request request{server.address("/a%20file.txt")};
    request.headers = {{"User-Agent", "Served Tests/1"}};
    const auto got = fetch(request);

    INFO(got.answer.problem);
    CHECK(got.answer.problem.empty());
    CHECK(got.answer.status == 200);
    CHECK(got.lastStatus == 200);
    CHECK(got.body == "the whole file");
    CHECK(asked.rfind("GET /a%20file.txt HTTP/1.1\r\n", 0) == 0);
    CHECK(asked.find("User-Agent: Served Tests/1\r\n") != std::string::npos);
}

TEST_CASE("Other statuses come with their bodies, and no problem", "[served]")
{
    if (!http::available())
        SKIP("This system can't fetch yet");

    TestServer server([](const std::string& request, const TestServer::Send& send) {
        if (request.find("/missing") != std::string::npos)
            send(answerWith("404 Not Found", "no such file"));
        else
            send(answerWith("500 Internal Server Error", ""));
    });

    const auto missing = fetch({server.address("/missing")});
    INFO(missing.answer.problem);
    CHECK(missing.answer.problem.empty());
    CHECK(missing.answer.status == 404);
    CHECK(missing.body == "no such file");

    const auto broken = fetch({server.address("/broken")});
    CHECK(broken.answer.problem.empty());
    CHECK(broken.answer.status == 500);
    CHECK(broken.body.empty());
}

TEST_CASE("A redirect is followed to what it names", "[served]")
{
    if (!http::available())
        SKIP("This system can't fetch yet");

    TestServer server([](const std::string& request, const TestServer::Send& send) {
        if (request.find("/moved") != std::string::npos)
            send(answerWith("302 Found", "", "Location: /here\r\n"));
        else
            send(answerWith("200 OK", "landed"));
    });

    const auto got = fetch({server.address("/moved")});
    INFO(got.answer.problem);
    CHECK(got.answer.problem.empty());
    CHECK(got.answer.status == 200);
    CHECK(got.body == "landed");
    CHECK(server.connections() == 2);
}

TEST_CASE("A long answer comes in order, piece by piece as it arrives", "[served]")
{
    if (!http::available())
        SKIP("This system can't fetch yet");

    std::string whole;

    for (int i = 0; i < 400000; ++i)
        whole += static_cast<char>('a' + i % 26);

    TestServer server([&whole](const std::string&, const TestServer::Send& send) {
        send("HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(whole.size()) + "\r\nConnection: close\r\n\r\n");

        for (std::size_t at = 0; at < whole.size(); at += 50000)
        {
            send(whole.substr(at, 50000));
            std::this_thread::sleep_for(5ms);
        }
    });

    const auto got = fetch({server.address("/long")});
    INFO(got.answer.problem);
    CHECK(got.answer.problem.empty());
    CHECK(got.answer.status == 200);
    CHECK(got.body == whole);
    CHECK(got.pieces > 1);
}

// A system's client may let go of what it was holding when the connection went, so nothing says how much came before.
TEST_CASE("A connection lost partway says so", "[served]")
{
    if (!http::available())
        SKIP("This system can't fetch yet");

    TestServer server([](const std::string&, const TestServer::Send& send) {
        send("HTTP/1.1 200 OK\r\nContent-Length: 100000\r\nConnection: close\r\n\r\n");
        send(std::string(1000, 'x'));
    });

    const auto got = fetch({server.address("/cut")});
    CHECK(got.answer.status == 200);
    CHECK_FALSE(got.answer.problem.empty());
    CHECK(got.body.size() <= 1000);
}

TEST_CASE("The fetch that stops partway leaves the rest unsent, with no problem and no wait", "[served]")
{
    if (!http::available())
        SKIP("This system can't fetch yet");

    // A minute's worth of answer, which the server stops sending only because the connection has gone.
    std::atomic<bool> sendFailed{false};
    TestServer server([&sendFailed](const std::string&, const TestServer::Send& send) {
        send("HTTP/1.1 200 OK\r\nContent-Length: 200000000\r\nConnection: close\r\n\r\n");
        const std::string block(1 << 16, 'x');

        for (int i = 0; i < 3000; ++i)
        {
            if (!send(block))
            {
                sendFailed = true;
                return;
            }

            std::this_thread::sleep_for(20ms);
        }
    });

    std::size_t received = 0;
    const auto started = std::chrono::steady_clock::now();
    const auto answer = http::get({server.address("/endless")}, [&received](int, std::string_view piece) {
        received += piece.size();
        return false;
    });
    const auto took = std::chrono::steady_clock::now() - started;

    INFO(answer.problem);
    CHECK(answer.problem.empty());
    CHECK(answer.status == 200);
    CHECK(received > 0);
    CHECK(took < 30s);

    for (int waited = 0; waited < 3000 && !sendFailed.load(); waited += 10)
        std::this_thread::sleep_for(10ms);

    CHECK(sendFailed.load());
}

TEST_CASE("A server that says nothing runs out the time given", "[served]")
{
    if (!http::available())
        SKIP("This system can't fetch yet");

    std::atomic<bool> release{false};
    TestServer server([&release](const std::string&, const TestServer::Send&) {
        while (!release.load())
            std::this_thread::sleep_for(10ms);
    });

    http::Request request{server.address("/silent")};
    request.timeout = 1s;
    const auto started = std::chrono::steady_clock::now();
    const auto got = fetch(request);
    const auto took = std::chrono::steady_clock::now() - started;
    release = true;

    CHECK(got.answer.status == 0);
    CHECK_FALSE(got.answer.problem.empty());
    CHECK(took >= 900ms);
    CHECK(took < 20s);
}
