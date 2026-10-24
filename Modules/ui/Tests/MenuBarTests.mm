#include <symbolengine/ui/MenuBar.h>

#include <choc/gui/choc_MessageLoop.h>

#include <catch2/catch_test_macros.hpp>

#import <AppKit/AppKit.h>

#include <string>

using namespace symbolengine;

namespace
{
std::string titleOf(NSMenuItem* item)
{
    return item.title.UTF8String != nullptr ? item.title.UTF8String : "";
}
}

TEST_CASE("The Mac's menu bar shows the program's menus, with their shortcuts, ticks and submenus, and runs what is chosen")
{
    choc::messageloop::initialise();

    int opened = 0, recent = -1, aboutShown = 0, quitAsked = 0, built = 0;
    bool snapping = true;

    {
        ui::MenuBar bar({{.title = "About the Test", .chosen = [&] { ++aboutShown; }}},
                        {{.title = "File",
                          .items =
                              [&] {
                                  ++built;
                                  return std::vector<ui::MenuItem>{
                                      {.title = "Open...", .shortcut = ui::Shortcut{'o'}, .chosen = [&] { ++opened; }},
                                      {.title = "Save As...", .shortcut = ui::Shortcut{'s', true}, .enabled = false},
                                      {},
                                      {.title = "Open Recent",
                                       .submenu = {{.title = "First", .chosen = [&] { recent = 0; }},
                                                   {.title = "Second", .chosen = [&] { recent = 1; }}}},
                                      {.title = "Snapping", .ticked = snapping, .chosen = [&] { snapping = !snapping; }}};
                              }}},
                        [&] { ++quitAsked; });

        NSMenu* main = [NSApp mainMenu];
        REQUIRE(main.numberOfItems == 2);
        NSMenu* file = [main itemAtIndex:1].submenu;
        REQUIRE(file != nil);
        CHECK(titleOf([main itemAtIndex:1]) == "File");
        REQUIRE(file.numberOfItems == 5);

        NSMenuItem* open = [file itemAtIndex:0];
        CHECK(titleOf(open) == "Open...");
        CHECK([open.keyEquivalent isEqualToString:@"o"]);
        CHECK(open.keyEquivalentModifierMask == NSEventModifierFlagCommand);

        NSMenuItem* saveAs = [file itemAtIndex:1];
        CHECK(!saveAs.enabled);
        CHECK(saveAs.keyEquivalentModifierMask == (NSEventModifierFlagCommand | NSEventModifierFlagShift));
        CHECK([file itemAtIndex:2].separatorItem);
        CHECK([file itemAtIndex:3].submenu.numberOfItems == 2);
        CHECK([file itemAtIndex:4].state == NSControlStateValueOn);

        // Chosen from the menu, from a submenu, and by its shortcut.
        [file performActionForItemAtIndex:0];
        CHECK(opened == 1);
        [[file itemAtIndex:3].submenu performActionForItemAtIndex:1];
        CHECK(recent == 1);

        NSEvent* commandO = [NSEvent keyEventWithType:NSEventTypeKeyDown
                                             location:NSZeroPoint
                                        modifierFlags:NSEventModifierFlagCommand
                                            timestamp:0
                                         windowNumber:0
                                              context:nil
                                           characters:@"o"
                                charactersIgnoringModifiers:@"o"
                                            isARepeat:NO
                                              keyCode:31];
        CHECK([main performKeyEquivalent:commandO]);
        CHECK(opened == 2);

        // What the menus show follows what changed, once they are told.
        [file performActionForItemAtIndex:4];
        CHECK(!snapping);
        const auto before = built;
        bar.changed();
        CHECK(built == before + 1);
        CHECK([[main itemAtIndex:1].submenu itemAtIndex:4].state == NSControlStateValueOff);

        // The app menu: the program's items first, and Quit last.
        NSMenu* app = [main itemAtIndex:0].submenu;
        CHECK(titleOf([app itemAtIndex:0]) == "About the Test");
        [app performActionForItemAtIndex:0];
        CHECK(aboutShown == 1);
        NSMenuItem* quit = [app itemAtIndex:app.numberOfItems - 1];
        CHECK([quit.keyEquivalent isEqualToString:@"q"]);
        [app performActionForItemAtIndex:app.numberOfItems - 1];
        CHECK(quitAsked == 1);
    }

    CHECK([NSApp mainMenu] == nil);
}
