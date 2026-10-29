#include <symbolengine/core/System.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string_view>
#include <thread>

using namespace symbolengine;

TEST_CASE("The program's architecture has one name on every system")
{
#if defined(__aarch64__) || defined(_M_ARM64)
    CHECK(system::architecture() == "arm64");
#elif defined(__x86_64__) || defined(_M_X64)
    CHECK(system::architecture() == "x86_64");
#elif defined(__arm__) || defined(_M_ARM)
    CHECK(system::architecture() == "arm");
#elif defined(__i386__) || defined(_M_IX86)
    CHECK(system::architecture() == "x86");
#endif
}

TEST_CASE("When the computer started is in the past and holds still")
{
    using namespace std::chrono;
    const auto started = system::computerStarted();
    const auto now = system_clock::now();
    CHECK(started < now);
    CHECK(started > now - hours(24 * 365));

    // Read again after a pause, it is the same moment, to within what Windows' estimate allows.
    std::this_thread::sleep_for(milliseconds(100));
    const auto later = system::computerStarted();
    CHECK(duration_cast<milliseconds>(later - started).count() < 1000);
    CHECK(duration_cast<milliseconds>(started - later).count() < 1000);
}

// Only refusals are checked here: an address that is opened opens the browser on the machine running the tests.
TEST_CASE("Only web addresses are opened in the browser")
{
    CHECK_FALSE(system::openInBrowser(""));
    CHECK_FALSE(system::openInBrowser("https://"));
    CHECK_FALSE(system::openInBrowser("file:///etc/hosts"));
    CHECK_FALSE(system::openInBrowser("javascript:alert(1)"));
    CHECK_FALSE(system::openInBrowser("/Applications/Calculator.app"));
    CHECK_FALSE(system::openInBrowser("ftp://example.com/"));
    CHECK_FALSE(system::openInBrowser("https://example.com/a b"));
    CHECK_FALSE(system::openInBrowser("https://example.com/\"; rm -rf /"));
}
