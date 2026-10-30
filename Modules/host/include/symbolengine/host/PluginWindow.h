#pragma once

#include "Plugin.h"

#include <symbolengine/ui/WebView.h>
#include <symbolengine/ui/Window.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace symbolengine
{

class PluginWindows;

/** A plugin's own window, as a DAW opens one: its editor in a window of the program's with the system's title bar,
    resizable only while the editor is and sized as the plugin asks, the editor in one view made with the window and
    kept for its life. It keeps its plugin while it is open. Made, used and destroyed on the main thread; built where
    the ui module is, on a Mac and on Windows. Destroying it closes the editor in order while the window is still whole,
    once no call into the plugin is under way, and lets go of the plugin a turn of the run loop later. */
class PluginWindow
{
public:
    struct Options
    {
        std::string title{};

        /** What shows where the editor hasn't drawn, as 0xRRGGBB. */
        std::uint32_t background = 0x000000;

        /** The program's plugin windows this one joins, kept in their place among the program's windows and taking
            the program's keys; none for a window left to the program alone. */
        PluginWindows* windows = nullptr;

        /** Where the plugin has no editor, or its editor can't be opened, the window shows a page of its parameters
            rather than not opening: SymbolEngine's own, plain and neutral, unless the program gives its own. */
        bool parameterPage = true;

        /** The program's own page of parameters, drawn as its other pages are, in place of SymbolEngine's: its files,
            served as a ui::WebView's are. It reaches the plugin through the functions SymbolEngine's page calls:
            "parameters", which gives each shown with its id, name, units, steps, value from 0 to 1, text and whether
            it is read-only; "stepTexts" with an id, the text of each of its steps; "begin" and "end" with an id
            around a gesture; and "set" with an id and a value, which gives its text; and it hears "values", each
            value changed elsewhere with its id and text, and "parameters" when they change. */
        ui::Resources page{};
    };

    /** Opens the plugin's editor in a window of its own, hidden, in the middle of the main screen; nothing, with the
        error set, if the plugin has no editor, one of its editors is open, a call into it is under way or its editor
        can't be opened. */
    static std::unique_ptr<PluginWindow> open(std::shared_ptr<Plugin> plugin, Options options, PluginError& error);

    ~PluginWindow();

    PluginWindow(const PluginWindow&) = delete;
    PluginWindow& operator=(const PluginWindow&) = delete;

    const std::shared_ptr<Plugin>& plugin() const;

    /** The editor's size, as it last said it or was told. */
    PluginEditorSize editorSize() const;

    /** Its editor, or null where the page of its parameters stands in for it. */
    PluginEditor* editor() const;

    /** The page of its parameters standing in for its editor, or null where its editor is shown, so a program giving
        its own page can reach it as any page of its own. */
    ui::WebView* parameterPage() const;

    /** Why the page of its parameters stands in for its editor: it has none, or its editor couldn't be opened. */
    PluginError editorProblem() const;

    /** Whether the user can resize it, as the editor says. */
    bool isResizable() const;

    void setTitle(const std::string& title);

    /** Shows it, without taking the keyboard from the window that has it, or hides it. */
    void setVisible(bool visible);
    bool isVisible() const;

    /** Shows it in front of the program's other windows, brings the program in front of others' too, and gives it the
        keyboard. */
    void toFront();

    /** Whether it has the keyboard. */
    bool isActive() const;

    /** Keeps it above every other program's windows, or lets it go behind them again. */
    void setAlwaysOnTop(bool onTop);

    /** Its place and the editor's size, as ui::Window::bounds gives them. */
    ui::Bounds bounds() const;

    /** Moves it, its top left to the place given as ui::Window::bounds measures it, at its size, held to the screen it
        overlaps most. */
    void moveTo(int x, int y);

    /** Puts it in the middle of the main screen. */
    void center();

    /** The NSWindow on a Mac, and the HWND on Windows. */
    void* nativeHandle() const;

    /** Its close button was pressed, once the click is over; the program closes the window by destroying it. */
    std::function<void()> onCloseButton;

    /** It gained or lost the keyboard. */
    std::function<void(bool active)> onActiveChanged;

    /** It was moved or resized, by the user, the program or the plugin. */
    std::function<void()> onMovedOrResized;

    struct Impl;

private:
    explicit PluginWindow(std::unique_ptr<Impl> impl);

    static std::unique_ptr<PluginWindow> openEditor(std::shared_ptr<Plugin> plugin, const Options& options,
                                                    PluginError& error);
    static std::unique_ptr<PluginWindow> openPage(std::shared_ptr<Plugin> plugin, const Options& options,
                                                  PluginError problem, PluginError& error);
    static std::unique_ptr<PluginWindow> finish(std::unique_ptr<PluginWindow> made, Impl& state, PluginWindows* group);

    std::unique_ptr<Impl> impl;
};

}
