#include <symbolengine/core/System.h>

#include <catch2/catch_test_macros.hpp>

using namespace symbolengine;

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
