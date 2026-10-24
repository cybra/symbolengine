#include <symbolengine/ui/WebView.h>
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

    #include <dwmapi.h>
#endif

#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <memory>
#include <string>

using namespace symbolengine;

namespace
{
// Runs the message loop until the condition holds or ten seconds pass, saying whether it held.
bool runUntil(const std::function<bool()>& condition)
{
    bool held = condition();
    choc::messageloop::Timer look(10, [&] {
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

// A page that reports its size when it loads and whenever it changes.
ui::Resources sizeReporter()
{
    return [](std::string_view path) -> std::optional<ui::Resource> {
        if (path != "/index.html")
            return std::nullopt;
        return ui::Resource{R"html(<!doctype html><html><head><meta charset="utf-8"><script src="symbolengine.js"></script>
<script>
const report = () => symbolengine.call("size", window.innerWidth, window.innerHeight);
window.addEventListener("load", report);
window.addEventListener("resize", report);
</script></head><body style="margin:0; background:#203040"></body></html>)html",
                            ""};
    };
}

// Presses the window's close button, as a click on it does.
void pressCloseButton(ui::Window& window)
{
#if defined(__APPLE__)
    choc::objc::call<void>(static_cast<id>(window.nativeHandle()), "performClose:", static_cast<id>(nullptr));
#else
    SendMessageW(static_cast<HWND>(window.nativeHandle()), WM_SYSCOMMAND, SC_CLOSE, 0);
#endif
}

// Whether its maximize button is there and works.
bool canMaximize(ui::Window& window)
{
#if defined(__APPLE__)
    const auto zoom = choc::objc::call<id>(static_cast<id>(window.nativeHandle()), "standardWindowButton:", 2);
    return zoom != nullptr && choc::objc::call<BOOL>(zoom, "isEnabled");
#else
    return (GetWindowLongPtrW(static_cast<HWND>(window.nativeHandle()), GWL_STYLE) & WS_MAXIMIZEBOX) != 0;
#endif
}

bool seeThrough(ui::Window& window)
{
#if defined(__APPLE__)
    return choc::objc::call<double>(static_cast<id>(window.nativeHandle()), "alphaValue") == 0.0;
#else
    DWORD cloaked = 0;
    DwmGetWindowAttribute(static_cast<HWND>(window.nativeHandle()), DWMWA_CLOAKED, &cloaked, sizeof cloaked);
    return cloaked != 0;
#endif
}

// Whether a native view is still there, out of every window.
bool standsAlone(void* view)
{
#if defined(__APPLE__)
    return choc::objc::call<id>(static_cast<id>(view), "superview") == nullptr;
#else
    return IsWindow(static_cast<HWND>(view)) && GetParent(static_cast<HWND>(view)) == nullptr;
#endif
}
}

TEST_CASE("A window holds a view that fills it, at the size asked for")
{
    choc::messageloop::initialise();

    ui::Window window({.title = "SymbolEngine window test", .size = {420, 300}, .minimumSize = {200, 150}});
    ui::WebView view({.resources = sizeReporter(), .name = "window-test"});
    REQUIRE(view.available());

    ui::Size reported;
    view.expose("size", [&](const Value& arguments) {
        reported = {static_cast<int>(arguments[0].intOr(0)), static_cast<int>(arguments[1].intOr(0))};
        return Value();
    });

    window.setContent(view.nativeView());
    window.setVisible(true);
    CHECK(window.isVisible());
    CHECK(window.size() == ui::Size{420, 300});
    CHECK(runUntil([&] { return reported == ui::Size{420, 300}; }));

    window.setSize({500, 360});
    CHECK(window.size() == ui::Size{500, 360});
    CHECK(runUntil([&] { return reported == ui::Size{500, 360}; }));
}

TEST_CASE("A window is held to its smallest size, and a place kept for it to a screen it can be reached on")
{
    choc::messageloop::initialise();

    ui::Window window({.size = {400, 300}, .minimumSize = {200, 150}});
    window.setSize({10, 10});
    CHECK(window.size() == ui::Size{200, 150});

    window.setSize({400, 300});
    const auto middle = window.bounds();
    const ui::Bounds moved{middle.x + 20, middle.y + 10, middle.width, middle.height};
    window.setBounds(moved);
    CHECK(window.bounds() == moved);

    // A place on a screen since unplugged.
    window.setBounds({-100000, -100000, middle.width, middle.height});
    const auto rescued = window.bounds();
    CHECK(rescued.x >= 0);
    CHECK(rescued.y >= 0);
    CHECK(rescued.width == middle.width);
    CHECK(rescued.height == middle.height);

    const auto area = window.workArea();
    CHECK(area.width >= rescued.width);
    CHECK(area.height >= rescued.height);
}

TEST_CASE("A window has a maximize button only if asked for")
{
    choc::messageloop::initialise();

    ui::Window plain({.size = {300, 200}});
    ui::Window maximizable({.size = {300, 200}, .maximizable = true});
    CHECK(!canMaximize(plain));
    CHECK(canMaximize(maximizable));
}

TEST_CASE("The close button leaves the window to the program, which may let go of it then")
{
    choc::messageloop::initialise();

    auto window = std::make_unique<ui::Window>(ui::Window::Options{.size = {300, 200}});
    window->setVisible(true);

    int pressed = 0;
    window->onCloseButton = [&] { ++pressed; };
    pressCloseButton(*window);
    CHECK(runUntil([&] { return pressed == 1; }));
    CHECK(window->isVisible());

    // The program may let go of the window then.
    window->onCloseButton = [&] {
        ++pressed;
        window.reset();
    };
    pressCloseButton(*window);
    CHECK(runUntil([&] { return window == nullptr; }));
    CHECK(pressed == 2);
}

TEST_CASE("A window shows see-through until told otherwise, and letting go of it leaves its view to its owner")
{
    choc::messageloop::initialise();

    ui::WebView view({.resources = sizeReporter(), .name = "window-test"});
    REQUIRE(view.available());

    {
        ui::Window window({.size = {300, 200}});
        window.setContent(view.nativeView());
        window.setSeeThrough(true);
        window.setVisible(true);
        CHECK(seeThrough(window));
        window.setSeeThrough(false);
        CHECK(!seeThrough(window));
        CHECK(!standsAlone(view.nativeView()));
    }

    CHECK(standsAlone(view.nativeView()));
}
