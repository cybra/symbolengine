#include <symbolengine/ui/Dialogs.h>
#include <symbolengine/ui/Window.h>

#include <choc/gui/choc_MessageLoop.h>

#if defined(__APPLE__)
    #include <choc/platform/choc_ObjectiveCHelpers.h>
#else
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>

    #include <commctrl.h>
#endif

#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <optional>
#include <string>

using namespace symbolengine;

namespace
{
// Runs the message loop until the condition holds or ten seconds pass, saying whether it held.
bool runUntil(const std::function<bool()>& condition)
{
    bool held = condition();
    choc::messageloop::Timer look(20, [&] {
        held = condition();
        if (held)
            choc::messageloop::stop();
        return !held;
    });
    choc::messageloop::Timer timeout(10000, [] {
        choc::messageloop::stop();
        return false;
    });
    if (!held)
        choc::messageloop::run();
    return held;
}

#if defined(__APPLE__)
// The dialog asking in front of the window, if one is, and the button in it with the title given.
id dialogOf(ui::Window& window)
{
    return choc::objc::call<id>(static_cast<id>(window.nativeHandle()), "attachedSheet");
}

id findButton(id view, const std::string& title)
{
    if (choc::objc::call<BOOL>(view, "isKindOfClass:", objc_getClass("NSButton")) &&
        choc::objc::getString(choc::objc::call<id>(view, "title")) == title)
        return view;

    id subviews = choc::objc::call<id>(view, "subviews");
    for (unsigned long i = 0; i < choc::objc::call<unsigned long>(subviews, "count"); ++i)
        if (id found = findButton(choc::objc::call<id>(subviews, "objectAtIndex:", i), title))
            return found;

    return nullptr;
}

// Presses the button with the title given in the dialog, saying whether it found one.
bool press(id dialog, const std::string& title, int)
{
    id button = findButton(choc::objc::call<id>(dialog, "contentView"), title);
    if (button == nullptr)
        return false;
    choc::objc::call<void>(button, "performClick:", static_cast<id>(nullptr));
    return true;
}
#else
HWND dialogOf(ui::Window& window)
{
    return GetWindow(static_cast<HWND>(window.nativeHandle()), GW_ENABLEDPOPUP);
}

// Presses the button the dialog numbers as given, its title being for the Mac's.
bool press(HWND dialog, const std::string&, int number)
{
    SendMessageW(dialog, TDM_CLICK_BUTTON, static_cast<WPARAM>(number), 0);
    return true;
}
#endif
}

TEST_CASE("A question is asked once the call has returned, in front of its window, and gives the button chosen")
{
    choc::messageloop::initialise();

    ui::Window window({.title = "SymbolEngine dialog test", .size = {500, 360}});
    window.setVisible(true);

    const ui::Question question{.title = "Save the changes?",
                                .message = "A question for the tests.",
                                .kind = ui::Question::Kind::warning,
                                .buttons = {"Save", "Cancel", "Don't Save"},
                                .cancel = 1};

    std::optional<std::size_t> answer;
    ui::ask(question, window.nativeHandle(), [&](std::size_t chosen) { answer = chosen; });
    CHECK(!answer.has_value());
    CHECK(dialogOf(window) == nullptr);

    // The third button, as Windows numbers it from 1000.
    bool pressed = false;
    CHECK(runUntil([&] {
        if (!pressed && dialogOf(window) != nullptr)
            pressed = press(dialogOf(window), "Don't Save", 1002);
        return answer.has_value();
    }));
    CHECK(answer == std::size_t{2});

#if defined(_WIN32)
    // Escape, or closing the dialog, chooses the cancel button.
    answer.reset();
    ui::ask(question, window.nativeHandle(), [&](std::size_t chosen) { answer = chosen; });
    pressed = false;
    CHECK(runUntil([&] {
        if (!pressed && dialogOf(window) != nullptr)
            pressed = press(dialogOf(window), "", IDCANCEL);
        return answer.has_value();
    }));
    CHECK(answer == std::size_t{1});
#endif
}
