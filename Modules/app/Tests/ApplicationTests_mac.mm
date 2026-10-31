#include <symbolengine/app/Application.h>

#include <symbolengine/core/Paths.h>
#include <symbolengine/ui/MessageLoop.h>

#include <catch2/catch_test_macros.hpp>

#import <AppKit/AppKit.h>

#include <chrono>
#include <fcntl.h>
#include <filesystem>
#include <string>
#include <sys/file.h>
#include <unistd.h>
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

namespace
{
// The event the system sends to quit a program, with the reason it gives on logging out when there is one, handed to the
// program's own handler as it would arrive. What the program answers goes in reply.
void sendQuit(NSAppleEventDescriptor* reply, OSType reason)
{
    NSAppleEventDescriptor* event = [NSAppleEventDescriptor appleEventWithEventClass:kCoreEventClass
                                                                             eventID:kAEQuitApplication
                                                                    targetDescriptor:[NSAppleEventDescriptor currentProcessDescriptor]
                                                                            returnID:kAutoGenerateReturnID
                                                                       transactionID:kAnyTransactionID];
    if (reason != 0)
        [event setAttributeDescriptor:[NSAppleEventDescriptor descriptorWithEnumCode:reason] forKeyword:kAEQuitReason];
    [[NSAppleEventManager sharedAppleEventManager] dispatchRawAppleEvent:event.aeDesc
                                                            withRawReply:const_cast<AEDesc*>(reply.aeDesc)
                                                           handlerRefCon:reply];
}

NSAppleEventDescriptor* emptyReply()
{
    return [NSAppleEventDescriptor appleEventWithEventClass:kCoreEventClass
                                                    eventID:kAEAnswer
                                           targetDescriptor:[NSAppleEventDescriptor currentProcessDescriptor]
                                                   returnID:kAutoGenerateReturnID
                                              transactionID:kAnyTransactionID];
}

std::int32_t errorIn(NSAppleEventDescriptor* reply)
{
    NSAppleEventDescriptor* number = [reply paramDescriptorForKeyword:keyErrorNumber];
    return number != nil ? number.int32Value : 0;
}
}

TEST_CASE("Logging out or shutting down waits for the program to end, and isn't refused for being asked")
{
    NSAppleEventDescriptor* reply = emptyReply();
    int asked = 0;
    {
        Application app({.name = "SymbolEngine app tests"}, 0, nullptr);
        app.onQuitRequested = [&] {
            ++asked;
            app.quit(7);
        };
        ui::messageLoop::callSoon([reply] { sendQuit(reply, kAELogOut); });
        CHECK(app.run() == 7);
        CHECK(asked == 1);
    }
    CHECK(errorIn(reply) == 0);
}

TEST_CASE("A program that decides against quitting says so at once, which stops the logging out")
{
    NSAppleEventDescriptor* reply = emptyReply();
    Application app({.name = "SymbolEngine app tests"}, 0, nullptr);
    app.onQuitRequested = [&] { app.declineQuit(); };

    bool sent = false;
    ui::messageLoop::callSoon([&, reply] {
        sendQuit(reply, kAEShutDown);
        sent = true;
    });
    CHECK(ui::messageLoop::runUntil([&] { return sent; }, std::chrono::seconds(5)));
    CHECK(errorIn(reply) == userCanceledErr);
    CHECK_FALSE(app.isQuitting());
}

TEST_CASE("A second instance isn't told it has handed over when no one took it")
{
    const auto identifier = "dev.symbolengine.tests.silent." + std::to_string(getpid());
    Application app({.name = "SymbolEngine app tests", .identifier = identifier}, 0, nullptr);

    // Held as a running instance holds it, with nothing listening.
    std::error_code ignored;
    const auto file = paths::systemFolder(paths::Folder::temp, ignored) / (identifier + ".instance");
    const int lock = open(file.c_str(), O_RDWR | O_CREAT, 0600);
    REQUIRE(lock >= 0);
    REQUIRE(flock(lock, LOCK_EX | LOCK_NB) == 0);

    CHECK_FALSE(app.sendToRunningInstance());

    close(lock);
    std::filesystem::remove(file, ignored);
}

TEST_CASE("A running instance doesn't take its orders from any other program's notification")
{
    const auto identifier = "dev.symbolengine.tests.heard." + std::to_string(getpid());
    Application app({.name = "SymbolEngine app tests", .identifier = identifier}, 0, nullptr);
    REQUIRE_FALSE(app.sendToRunningInstance());

    bool heard = false;
    app.onAnotherInstance = [&](std::vector<std::string>) { heard = true; };

    NSString* name = [NSString stringWithUTF8String:(identifier + ".symbolengine.instance").c_str()];
    [[NSDistributedNotificationCenter defaultCenter] postNotificationName:name
                                                                   object:nil
                                                                 userInfo:@{@"arguments" : @[ @"/tmp/not.wav" ]}
                                                       deliverImmediately:YES];
    ui::messageLoop::runUntil([&] { return heard; }, std::chrono::milliseconds(500));
    CHECK_FALSE(heard);
}
