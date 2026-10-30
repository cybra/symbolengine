#include "symbolengine/core/System.h"
#include "symbolengine/net/Http.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

using namespace symbolengine;

namespace
{

// Nothing should come from these, so anything that does fails the test.
const auto refused = [](int, std::string_view) {
    FAIL("Something arrived from an address that has nothing there");
    return false;
};

// An address that answers, for the tests that need a server: a small public file, set where the machine has the internet.
std::string fileOnTheWeb()
{
    return system::environmentVariable("SYMBOLENGINE_HTTP_TEST_URL").value_or("");
}

}

TEST_CASE("A part of an address has everything escaped but what needs no escaping")
{
    CHECK(http::escaped("Symbol-Studio_0.2.3~a") == "Symbol-Studio_0.2.3~a");
    CHECK(http::escaped("a b/c+d") == "a%20b%2Fc%2Bd");
    CHECK(http::escaped("caf\xc3\xa9") == "caf%C3%A9");
    CHECK(http::escaped("") == "");
}

TEST_CASE("A file's address names the file, whatever is in its name")
{
#if defined(_WIN32)
    const std::filesystem::path file = L"C:\\Users\\a b\\caf\u00e9.txt";
    CHECK(http::fileAddress(file) == "file:///C:/Users/a%20b/caf%C3%A9.txt");
#else
    const std::filesystem::path file = "/Users/a b/caf\xc3\xa9.txt";
    CHECK(http::fileAddress(file) == "file:///Users/a%20b/caf%C3%A9.txt");
#endif
    const auto back = http::fileAt(http::fileAddress(file));
    REQUIRE(back.has_value());
    CHECK(*back == file);
}

TEST_CASE("Only a file address names a file")
{
    CHECK_FALSE(http::fileAt("https://example.com/a").has_value());
    CHECK_FALSE(http::fileAt("").has_value());
}

TEST_CASE("Asking somewhere that isn't a web address says so")
{
    for (const auto* address : {"", "nonsense", "file:///etc/hosts", "ftp://example.com/a"})
    {
        const auto answer = http::get({address}, refused);
        CHECK(answer.status == 0);
        CHECK_FALSE(answer.problem.empty());
    }
}

TEST_CASE("Asking where nothing listens says so, with no status")
{
    http::Request request{"http://127.0.0.1:9/nothing"};
    request.timeout = std::chrono::seconds(10);
    const auto answer = http::get(request, refused);
    CHECK(answer.status == 0);
    CHECK_FALSE(answer.problem.empty());
}

TEST_CASE("Asking for a name that doesn't exist says so, with no status")
{
    http::Request request{"https://nothing-here.invalid/"};
    request.timeout = std::chrono::seconds(10);
    const auto answer = http::get(request, refused);
    CHECK(answer.status == 0);
    CHECK_FALSE(answer.problem.empty());
}

TEST_CASE("A file on the web comes whole, with its status", "[internet]")
{
    const auto address = fileOnTheWeb();

    if (address.empty() || !http::available())
        SKIP("SYMBOLENGINE_HTTP_TEST_URL isn't set, or this system can't fetch yet");

    std::string body;
    int seen = 0;
    http::Request request{address};
    request.headers = {{"User-Agent", "SymbolEngine tests"}};
    const auto answer = http::get(request, [&](int status, std::string_view piece) {
        seen = status;
        body += piece;
        return true;
    });

    INFO(answer.problem);
    CHECK(answer.problem.empty());
    CHECK(answer.status == 200);
    CHECK(seen == 200);
    CHECK(body.find("MIT License") != std::string::npos);
}

TEST_CASE("A file not on the web answers with its status, no problem", "[internet]")
{
    const auto address = fileOnTheWeb();

    if (address.empty() || !http::available())
        SKIP("SYMBOLENGINE_HTTP_TEST_URL isn't set, or this system can't fetch yet");

    const auto answer = http::get({address + "-not-there"}, [](int, std::string_view) { return true; });
    INFO(answer.problem);
    CHECK(answer.problem.empty());
    CHECK(answer.status == 404);
}

TEST_CASE("Stopping partway stops the fetch, with no problem", "[internet]")
{
    const auto address = fileOnTheWeb();

    if (address.empty() || !http::available())
        SKIP("SYMBOLENGINE_HTTP_TEST_URL isn't set, or this system can't fetch yet");

    int pieces = 0;
    const auto answer = http::get({address}, [&](int, std::string_view) {
        ++pieces;
        return false;
    });

    INFO(answer.problem);
    CHECK(pieces == 1);
    CHECK(answer.problem.empty());
    CHECK(answer.status == 200);
}
