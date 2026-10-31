// What a web view does on a Mac that only a Mac's own classes can show. The key events here are made and handed to the view
// directly, so nothing reaches the keyboard or the front of the screen.

#include <symbolengine/ui/Dialogs.h>
#include <symbolengine/ui/MessageLoop.h>
#include <symbolengine/ui/WebView.h>

#include <catch2/catch_test_macros.hpp>

#import <AppKit/AppKit.h>

#include <objc/message.h>

#include <chrono>
#include <csignal>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace symbolengine;

namespace
{
ui::Resources onePage(std::string page)
{
    return [page = std::move(page)](std::string_view path) -> std::optional<ui::Resource> {
        if (path == "/index.html")
            return ui::Resource{page, "text/html"};
        return std::nullopt;
    };
}

NSEvent* commandKey(NSString* key)
{
    return [NSEvent keyEventWithType:NSEventTypeKeyDown
                            location:NSZeroPoint
                       modifierFlags:NSEventModifierFlagCommand
                           timestamp:0
                        windowNumber:0
                             context:nil
                          characters:key
         charactersIgnoringModifiers:key
                           isARepeat:NO
                             keyCode:0];
}

// A window never shown, for what depends on having one.
NSWindow* unseenWindow()
{
    return [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 400, 300)
                                       styleMask:NSWindowStyleMaskBorderless
                                         backing:NSBackingStoreBuffered
                                           defer:YES];
}

NSButton* buttonIn(NSView* view, NSString* title)
{
    if ([view isKindOfClass:[NSButton class]] && [static_cast<NSButton*>(view).title isEqualToString:title])
        return static_cast<NSButton*>(view);
    for (NSView* inside in view.subviews)
        if (NSButton* found = buttonIn(inside, title))
            return found;
    return nil;
}
}

TEST_CASE("Command-C and the like are the page's only while it has the keyboard", "[webview]")
{
    ui::messageLoop::initialize();
    ui::WebView view({.resources = onePage("<!doctype html><p>A page for the tests</p>")});
    REQUIRE(view.available());

    auto* web = static_cast<NSView*>(view.nativeView());
    NSWindow* window = unseenWindow();
    NSTextField* field = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 100, 20)];
    [window.contentView addSubview:web];
    [window.contentView addSubview:field];

    // The host's own field has the keyboard, so the key is left for the host.
    REQUIRE([window makeFirstResponder:field]);
    CHECK_FALSE([web performKeyEquivalent:commandKey(@"c")]);

    REQUIRE([window makeFirstResponder:web]);
    CHECK([web performKeyEquivalent:commandKey(@"c")]);

    [web removeFromSuperview];
    [window close];
}

TEST_CASE("A page's context menu has no way of leaving the page, unless it is for development", "[webview]")
{
    ui::messageLoop::initialize();

    const auto left = [](bool developerTools) {
        ui::WebView view({.resources = onePage("<!doctype html><p>A page for the tests</p>"), .developerTools = developerTools});
        REQUIRE(view.available());
        NSMenu* menu = [[NSMenu alloc] init];
        for (NSString* identifier in @[ @"WKMenuItemIdentifierCopy", @"WKMenuItemIdentifierReload", @"WKMenuItemIdentifierOpenLink" ])
        {
            NSMenuItem* item = [menu addItemWithTitle:identifier action:nil keyEquivalent:@""];
            item.identifier = identifier;
        }
        NSEvent* click = [NSEvent mouseEventWithType:NSEventTypeRightMouseDown
                                            location:NSZeroPoint
                                       modifierFlags:0
                                           timestamp:0
                                        windowNumber:0
                                             context:nil
                                         eventNumber:0
                                          clickCount:1
                                            pressure:1.0];
        [static_cast<NSView*>(view.nativeView()) willOpenMenu:menu withEvent:click];
        std::vector<std::string> identifiers;
        for (NSMenuItem* item in menu.itemArray)
            identifiers.emplace_back(item.identifier.UTF8String);
        return identifiers;
    };

    CHECK(left(false) == std::vector<std::string>{"WKMenuItemIdentifierCopy"});
    CHECK(left(true).size() == 3);
}

TEST_CASE("A page whose renderer dies is loaded again, and what the program sends then reaches it", "[webview]")
{
    ui::messageLoop::initialize();
    int loads = 0;
    std::string heard;

    ui::WebView view({.resources = onePage(R"html(<!doctype html>
<script src="symbolengine.js"></script>
<script>
symbolengine.on("hello", text => symbolengine.call("heard", text));
symbolengine.call("loaded");
</script>)html")});
    REQUIRE(view.available());
    view.expose("loaded", [&](const Value&) {
        ++loads;
        return Value();
    });
    view.expose("heard", [&](const Value& arguments) {
        heard = arguments[0].stringOr("");
        return Value();
    });
    REQUIRE(ui::messageLoop::runUntil([&] { return loads == 1 && view.pageReady(); }, std::chrono::seconds(20)));

    // The web view's own process for this page, which dies as a crash would.
    id web = static_cast<id>(view.nativeView());
    const SEL process = NSSelectorFromString(@"_webProcessIdentifier");
    REQUIRE([web respondsToSelector:process]);
    const auto renderer = reinterpret_cast<pid_t (*)(id, SEL)>(objc_msgSend)(web, process);
    REQUIRE(renderer > 0);
    REQUIRE(kill(renderer, SIGKILL) == 0);

    // Held, not lost, while there is no page to take it.
    REQUIRE(ui::messageLoop::runUntil([&] { return !view.pageReady(); }, std::chrono::seconds(10)));
    view.send("hello", "again");
    CHECK(ui::messageLoop::runUntil([&] { return loads == 2 && heard == "again"; }, std::chrono::seconds(20)));
}

TEST_CASE("While a question is up in a window of its own, what is posted to the loop still runs", "[webview]")
{
    ui::messageLoop::initialize();

    std::optional<std::size_t> answer;
    ui::ask({.title = "Save the changes?", .message = "A question for the tests.", .buttons = {"Stay"}}, nullptr,
            [&](std::size_t chosen) { answer = chosen; });

    // A timer of the run loop's own, in the modes a modal window runs in, whatever becomes of the main queue.
    struct Seen
    {
        bool posted = false, ran = false;
        int ticksSincePosted = 0;
    };
    const auto seen = std::make_shared<Seen>();
    NSTimer* look = [NSTimer timerWithTimeInterval:0.05
                                           repeats:YES
                                             block:^(NSTimer*) {
                                               NSWindow* modal = [NSApp modalWindow];
                                               if (modal == nil)
                                                   return;
                                               if (!seen->posted)
                                               {
                                                   seen->posted = true;
                                                   ui::messageLoop::callAsync([seen] { seen->ran = true; });
                                               }
                                               else if (seen->ran || ++seen->ticksSincePosted > 40)
                                                   [buttonIn(modal.contentView, @"Stay") performClick:nil];
                                             }];
    [[NSRunLoop mainRunLoop] addTimer:look forMode:NSRunLoopCommonModes];

    CHECK(ui::messageLoop::runUntil([&] { return answer.has_value(); }, std::chrono::seconds(15)));
    [look invalidate];
    CHECK(seen->posted);
    CHECK(seen->ran);
}
