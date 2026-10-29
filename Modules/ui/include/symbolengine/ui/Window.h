#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace symbolengine::ui
{

/** A window's size: in points on a Mac, and on Windows in pixels at 100% scaling, which the window turns into the
    pixels of the screen it is on, so it looks the same size on every screen. */
struct Size
{
    int width = 0;
    int height = 0;

    bool operator==(const Size&) const = default;
};

/** A window's place and size on the screens, for keeping where it was left: in points on a Mac, and in the screens'
    own pixels on Windows, measured from the top left of the main screen. */
struct Bounds
{
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    bool operator==(const Bounds&) const = default;
};

/** How a window's title bar is drawn, and so the look a web view in it reports to its page: as the computer is set,
    light or dark. */
enum class Appearance
{
    system,
    light,
    dark
};

/** A window of the program's own, with the system's title bar and its close and minimize buttons, and its maximize
    button if asked for, holding one native view, such as a WebView's, which fills it. Its size and place are those of
    what it holds, the title bar left out. Made, used and destroyed on the message thread. */
class Window
{
public:
    struct Options
    {
        std::string title{};
        Size size{800, 600};
        Size minimumSize{100, 100};
        bool resizable = true;

        /** With the maximize button: a Mac's green button, which takes the window full screen, and Windows'. */
        bool maximizable = false;

        /** What shows where the view hasn't drawn, as 0xRRGGBB, such as while the window is being resized. */
        std::uint32_t background = 0x000000;

        /** On Windows, measures its size in the pixels of the screen it is on rather than at 100% scaling, as a window
            holding something that sizes itself in pixels does: size(), setSize(), workArea(), its smallest size and
            onResizing are in pixels, and moved to a screen of another scale it keeps its size in pixels. On a Mac
            sizes are points either way. */
        bool sizedInPixels = false;
    };

    /** Makes the window, hidden, in the middle of the main screen. */
    explicit Window(Options options);
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    /** Puts the view in the window, filling it, in place of any view it held. The window doesn't own it. */
    void setContent(void* view);

    void setTitle(const std::string& title);

    /** Shows it, without taking the keyboard from the window that has it, or hides it. */
    void setVisible(bool visible);
    bool isVisible() const;

    /** Shows it in front of the program's other windows, brings the program in front of others' too, and gives it the
        keyboard. */
    void toFront();

    /** Whether it has the keyboard. */
    bool isActive() const;

    bool isMinimized() const;

    /** Keeps it above every other program's windows, or lets it go behind them again. */
    void setAlwaysOnTop(bool onTop);

    /** Lets the user resize it by its edges, or stops them. */
    void setResizable(bool resizable);
    bool isResizable() const;

    /** Leaves it see-through while it shows, or makes it opaque again, as for a page still drawing its first frame. */
    void setSeeThrough(bool seeThrough);

    void setAppearance(Appearance appearance);

    Size size() const;

    /** Sets its size, keeping its top left where it is, held to its smallest size. */
    void setSize(Size size);

    /** The part of the screen it is on that windows use, without the menu bar and the Dock, or the taskbar. */
    Size workArea() const;

    /** The scale of the screen it is on: its pixels to a point on a Mac, as 2 on a Retina screen, and its DPI over 96
        on Windows, as 1.5 at 150%. */
    double scale() const;

    Bounds bounds() const;

    /** Sets its place and size, held to its smallest size and to the screen it overlaps most, or the main screen when
        it overlaps none, so a place kept on a screen since unplugged still opens where it can be reached. */
    void setBounds(Bounds bounds);

    /** Puts it in the middle of the main screen, at its size. */
    void center();

    /** Has it fill the screen it is on, or brings it back from that: maximized on Windows, and on a Mac filling the
        screen beside the Dock and under the menu bar, as a double-click on its title bar does. */
    void setMaximized(bool maximized);
    bool isMaximized() const;

    /** Where it was, and how large, the last time it wasn't maximized or minimized, as bounds() gives it, for keeping
        between launches. */
    Bounds restoredBounds() const;

    /** On Windows 11, colors its title bar, as 0xRRGGBB, as a program whose window starts with a bar of its own color
        does; elsewhere, and on a Mac, it does nothing. */
    void setTitleBarColor(std::uint32_t background, std::uint32_t text);

    /** Gives the view it holds the keyboard, as after a dialog has had it. */
    void focusContent();

    /** On a Mac, shows a dot in its close button, as a window holding changes not yet saved does; elsewhere it does
        nothing. */
    void setEdited(bool edited);

    /** The NSWindow on a Mac, and the HWND on Windows. */
    void* nativeHandle() const;

    /** Its close button was pressed. The window stays as it is for the program to hide or destroy, which it may do
        here: this is called once the click is over. */
    std::function<void()> onCloseButton;

    /** It gained or lost the keyboard. */
    std::function<void(bool active)> onActiveChanged;

    /** It was moved or resized, by the user or the program. */
    std::function<void()> onMovedOrResized;

    /** While the user drags its edge: given the size it is being dragged to, as size() measures it, gives the size it
        takes instead, as a window holding something with sizes of its own does. Without it, it takes the size dragged
        to. */
    std::function<Size(Size)> onResizing;

    /** Its scale changed, as when it is moved to a screen of another scale: told once the user's move is over, so
        what it holds is scaled once rather than at every step of the move. */
    std::function<void()> onScaleChanged;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}
