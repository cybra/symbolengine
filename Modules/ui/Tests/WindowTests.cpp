#include <symbolengine/ui/WebView.h>
#include <symbolengine/ui/Window.h>

#include <choc/gui/choc_MessageLoop.h>

#if defined(__APPLE__)
    #include <choc/platform/choc_ObjectiveCHelpers.h>

    #include <CoreGraphics/CoreGraphics.h>
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

#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <vector>

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

// Whether the system lets the user resize it by its edges.
bool userCanResize(ui::Window& window)
{
#if defined(__APPLE__)
    const auto mask = choc::objc::call<unsigned long>(static_cast<id>(window.nativeHandle()), "styleMask");
    return (mask & (1ul << 3)) != 0;   // NSWindowStyleMaskResizable
#else
    return (GetWindowLongPtrW(static_cast<HWND>(window.nativeHandle()), GWL_STYLE) & WS_THICKFRAME) != 0;
#endif
}

// The user dragging the window's bottom right corner toward a size, as size() measures it: the system asks the window
// what size it takes, as it does at each step of a drag, and this gives the answer, as size() would measure it.
ui::Size dragTo(ui::Window& window, ui::Size size)
{
#if defined(__APPLE__)
    const auto handle = static_cast<id>(window.nativeHandle());
    const auto frame =
        choc::objc::call<CGRect>(handle, "frameRectForContentRect:", CGRectMake(0, 0, size.width, size.height));
    const auto delegate = choc::objc::call<id>(handle, "delegate");
    auto taken = frame.size;
    if (delegate != nullptr &&
        choc::objc::call<BOOL>(delegate, "respondsToSelector:", sel_registerName("windowWillResize:toSize:")))
        taken = choc::objc::call<CGSize>(delegate, "windowWillResize:toSize:", handle, frame.size);
    const auto content =
        choc::objc::call<CGRect>(handle, "contentRectForFrameRect:", CGRectMake(0, 0, taken.width, taken.height));
    return {static_cast<int>(std::lround(content.size.width)), static_cast<int>(std::lround(content.size.height))};
#else
    const auto handle = static_cast<HWND>(window.nativeHandle());
    RECT outer{}, inner{};
    GetWindowRect(handle, &outer);
    GetClientRect(handle, &inner);
    const double scale = GetDpiForWindow(handle) / 96.0;
    const int edgesWide = (outer.right - outer.left) - inner.right;
    const int edgesHigh = (outer.bottom - outer.top) - inner.bottom;
    RECT dragged{outer.left, outer.top, outer.left + edgesWide + static_cast<int>(std::lround(size.width * scale)),
                 outer.top + edgesHigh + static_cast<int>(std::lround(size.height * scale))};
    SendMessageW(handle, WM_SIZING, WMSZ_BOTTOMRIGHT, reinterpret_cast<LPARAM>(&dragged));
    return {static_cast<int>(std::lround((dragged.right - dragged.left - edgesWide) / scale)),
            static_cast<int>(std::lround((dragged.bottom - dragged.top - edgesHigh) / scale))};
#endif
}

// The system telling the window its scale changed, as when the user drags it to a screen of another scale: on Windows
// the new DPI, with the place Windows suggests, the same size at 100% on the new screen; on a Mac the notice that its
// backing scale changed. The user's move around it, on Windows, begun and ended.
#if defined(__APPLE__)
void rescale(ui::Window& window, unsigned)
{
    const auto center = choc::objc::callClass<id>("NSNotificationCenter", "defaultCenter");
    choc::objc::call<void>(center, "postNotificationName:object:",
                           choc::objc::getNSString("NSWindowDidChangeBackingPropertiesNotification"),
                           static_cast<id>(window.nativeHandle()));
}

double systemScale(ui::Window& window)
{
    return choc::objc::call<double>(static_cast<id>(window.nativeHandle()), "backingScaleFactor");
}
#else
// The window's client area in its screen's own pixels.
ui::Size clientPixels(ui::Window& window)
{
    RECT client{};
    GetClientRect(static_cast<HWND>(window.nativeHandle()), &client);
    return {client.right, client.bottom};
}

// The place suggested holds the client area scaled to the new DPI inside the frame the window has: a test's stand-in
// for a real screen of that scale, whose frame Windows would draw at it.
void rescale(ui::Window& window, unsigned dpi)
{
    const auto handle = static_cast<HWND>(window.nativeHandle());
    RECT outer{}, client{};
    GetWindowRect(handle, &outer);
    GetClientRect(handle, &client);
    const auto before = static_cast<int>(std::lround(window.scale() * 96.0));
    const int edgesWide = (outer.right - outer.left) - client.right;
    const int edgesHigh = (outer.bottom - outer.top) - client.bottom;
    const RECT suggested{outer.left, outer.top,
                         outer.left + edgesWide + MulDiv(client.right, static_cast<int>(dpi), before),
                         outer.top + edgesHigh + MulDiv(client.bottom, static_cast<int>(dpi), before)};
    SendMessageW(handle, WM_DPICHANGED, MAKEWPARAM(dpi, dpi), reinterpret_cast<LPARAM>(&suggested));
}

void beginMove(ui::Window& window)
{
    SendMessageW(static_cast<HWND>(window.nativeHandle()), WM_ENTERSIZEMOVE, 0, 0);
}

void endMove(ui::Window& window)
{
    SendMessageW(static_cast<HWND>(window.nativeHandle()), WM_EXITSIZEMOVE, 0, 0);
}

double systemScale(ui::Window& window)
{
    return GetDpiForWindow(static_cast<HWND>(window.nativeHandle())) / 96.0;
}
#endif

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

TEST_CASE("A window holds a view that fills it, at the size asked for", "[webview]")
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

TEST_CASE("A window the user drags takes the size the program gives it, and can be made fixed and resizable again")
{
    choc::messageloop::initialise();

    ui::Window window({.size = {400, 300}, .minimumSize = {100, 100}});
    CHECK(window.isResizable());
    CHECK(userCanResize(window));
    CHECK(dragTo(window, {500, 380}) == ui::Size{500, 380});

    // Held to whole hundreds, as a program holding something with sizes of its own might.
    std::vector<ui::Size> asked;
    window.onResizing = [&](ui::Size wanted) {
        asked.push_back(wanted);
        return ui::Size{wanted.width / 100 * 100, wanted.height / 100 * 100};
    };
    CHECK(dragTo(window, {530, 390}) == ui::Size{500, 300});
    REQUIRE(asked.size() == 1);
    CHECK(asked[0] == ui::Size{530, 390});

    window.setResizable(false);
    CHECK(!window.isResizable());
    CHECK(!userCanResize(window));
    CHECK(window.size() == ui::Size{400, 300});

    window.setResizable(true);
    CHECK(window.isResizable());
    CHECK(userCanResize(window));
}

TEST_CASE("A window says when its scale changes, once the user's move is over, and one measured in pixels keeps its "
          "size in pixels")
{
    choc::messageloop::initialise();

    ui::Window window({.size = {600, 400}, .minimumSize = {100, 50}, .sizedInPixels = true});
    CHECK(window.scale() == systemScale(window));
    int told = 0;
    window.onScaleChanged = [&] { ++told; };

#if defined(__APPLE__)
    rescale(window, 0);
    CHECK(told == 1);
#else
    // Measured in pixels: the size asked for, in pixels, whatever the scale, and kept on a screen of another.
    CHECK(clientPixels(window) == ui::Size{600, 400});
    CHECK(window.size() == ui::Size{600, 400});
    const auto twice = static_cast<unsigned>(std::lround(window.scale() * 2 * 96.0));
    beginMove(window);
    rescale(window, twice);
    CHECK(told == 0);
    CHECK(window.scale() == twice / 96.0);
    CHECK(clientPixels(window) == ui::Size{600, 400});
    CHECK(window.size() == ui::Size{600, 400});
    endMove(window);
    CHECK(told == 1);

    // Measured at 100%: the same size in those, its pixels following the scale, told at once when nobody moves it.
    ui::Window units({.size = {300, 200}});
    const auto pixels = clientPixels(units);
    int unitsTold = 0;
    units.onScaleChanged = [&] { ++unitsTold; };
    rescale(units, static_cast<unsigned>(std::lround(units.scale() * 2 * 96.0)));
    CHECK(unitsTold == 1);
    CHECK(units.size() == ui::Size{300, 200});
    CHECK(clientPixels(units) == ui::Size{pixels.width * 2, pixels.height * 2});
#endif
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

TEST_CASE("A window shows see-through until told otherwise, and letting go of it leaves its view to its owner", "[webview]")
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

TEST_CASE("A maximized window fills the screen it is on, and goes back to where it was")
{
    choc::messageloop::initialise();

    ui::Window window({.size = {400, 300}, .maximizable = true});
    window.setVisible(true);
    const auto before = window.bounds();
    CHECK(!window.isMaximized());
    CHECK(window.restoredBounds() == before);

    window.setMaximized(true);
    CHECK(runUntil([&] { return window.isMaximized(); }));
    const auto area = window.workArea();
    CHECK(window.size().width > before.width);
    CHECK(window.size().height > before.height);
    CHECK(window.size().width <= area.width);
    CHECK(window.restoredBounds() == before);

    window.setMaximized(false);
    CHECK(runUntil([&] { return !window.isMaximized(); }));
    CHECK(window.bounds() == before);

    // Placed while hidden and maximized before it shows, as a program opening where it was left does.
    ui::Window reopened({.size = {400, 300}, .maximizable = true});
    const ui::Bounds left{before.x + 30, before.y + 20, 420, 310};
    reopened.setBounds(left);
    reopened.setMaximized(true);
    reopened.setVisible(true);
    CHECK(runUntil([&] { return reopened.isMaximized(); }));
    CHECK(reopened.restoredBounds() == left);
    reopened.setMaximized(false);
    CHECK(runUntil([&] { return !reopened.isMaximized(); }));
    CHECK(reopened.bounds() == left);
}

#if defined(__APPLE__)
TEST_CASE("A window's view has the keyboard whenever the window does", "[webview]")
{
    choc::messageloop::initialise();

    ui::WebView view({.resources = sizeReporter(), .name = "window-test"});
    REQUIRE(view.available());
    ui::Window window({.size = {300, 200}});
    window.setContent(view.nativeView());
    const auto handle = static_cast<id>(window.nativeHandle());
    CHECK(choc::objc::call<id>(handle, "firstResponder") == static_cast<id>(view.nativeView()));

    choc::objc::call<BOOL>(handle, "makeFirstResponder:", static_cast<id>(nullptr));
    CHECK(choc::objc::call<id>(handle, "firstResponder") != static_cast<id>(view.nativeView()));
    window.focusContent();
    CHECK(choc::objc::call<id>(handle, "firstResponder") == static_cast<id>(view.nativeView()));
}

TEST_CASE("A Mac window shows when it holds changes not yet saved")
{
    choc::messageloop::initialise();

    ui::Window window({.size = {300, 200}});
    const auto handle = static_cast<id>(window.nativeHandle());
    CHECK(!choc::objc::call<BOOL>(handle, "isDocumentEdited"));
    window.setEdited(true);
    CHECK(choc::objc::call<BOOL>(handle, "isDocumentEdited"));
    window.setEdited(false);
    CHECK(!choc::objc::call<BOOL>(handle, "isDocumentEdited"));
}
#endif
