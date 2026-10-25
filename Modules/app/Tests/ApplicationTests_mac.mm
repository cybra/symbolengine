#include <symbolengine/app/Application.h>

#include <symbolengine/core/Paths.h>

#include <catch2/catch_test_macros.hpp>

#import <AppKit/AppKit.h>

#include <filesystem>
#include <vector>

using namespace symbolengine;

TEST_CASE("On a Mac the system's requests reach the Application through its delegate")
{
    Application app({.name = "SymbolEngine app tests"}, 0, nullptr);
    id<NSApplicationDelegate> delegate = [NSApp delegate];
    REQUIRE(delegate != nil);

    // Files to open, as the Finder hands them over, as paths.
    std::vector<std::filesystem::path> opened;
    app.onOpenFiles = [&](std::vector<std::filesystem::path> files) { opened = std::move(files); };
    [delegate application:NSApp openFiles:@[ @"/tmp/take one.wav", @"/tmp/Straße.flac" ]];
    REQUIRE(opened.size() == 2);
    CHECK(paths::toUtf8(opened[0]) == "/tmp/take one.wav");
    CHECK(paths::toUtf8(opened[1]) == "/tmp/Stra\xc3\x9f" "e.flac");

    // Quitting from the Dock or at logout asks the program, and the system is told to wait for it.
    int asked = 0;
    app.onQuitRequested = [&] { ++asked; };
    CHECK([delegate applicationShouldTerminate:NSApp] == NSTerminateCancel);
    CHECK(asked == 1);
    CHECK_FALSE(app.isQuitting());

    // The Dock icon clicked with no window showing.
    int reopened = 0;
    app.onReopen = [&] { ++reopened; };
    CHECK([delegate applicationShouldHandleReopen:NSApp hasVisibleWindows:NO] == YES);
    CHECK(reopened == 1);
    CHECK([delegate applicationShouldHandleReopen:NSApp hasVisibleWindows:YES] == YES);
    CHECK(reopened == 1);
}
