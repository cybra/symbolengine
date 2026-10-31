#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace symbolengine
{

/** A desktop program: the system's application object, the message loop, what the system asks of a running program,
    and the program's other instances. One per program, made first on the main thread, before any window. The program
    then sets the callbacks below, makes its windows and calls run(), which returns the code given to quit() once the
    loop has stopped, for main to return; its windows are destroyed after run() returns, while the Application is
    still alive.

        int main(int argc, char** argv)
        {
            Application app({.name = "Editor", .identifier = "com.example.editor"}, argc, argv);
            if (app.sendToRunningInstance())
                return 0;
            ui::Window window({.title = "Editor"});
            app.onQuitRequested = [&] { app.quit(); };
            window.setVisible(true);
            return app.run();
        }
*/
class Application
{
public:
    struct Options
    {
        /** The program's name, as the system shows it. */
        std::string name{};

        /** A name of the program's own among every program on the computer, such as com.example.editor, by which
            one instance of it finds another. The name when empty. */
        std::string identifier{};
    };

    /** On Windows the arguments come from the command line as the system has it, in UTF-16, so names outside the
        system's code page arrive whole; argc and argv are what main was given, and are read on the other systems. */
    Application(Options options, int argc, char** argv);
    ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    /** The one made, or nothing before it is made or after it is destroyed. */
    static Application* current() noexcept;

    const Options& options() const noexcept;

    /** The arguments the program was started with, as UTF-8, without the program's own path. */
    const std::vector<std::string>& arguments() const noexcept;

    /** Hands the arguments to an instance of this program already running, which hears them as onAnotherInstance, and
        says whether one took them: a program that runs one instance at a time returns from main when this is true.
        When no instance is running, this one becomes the running instance others find, until it is destroyed. */
    bool sendToRunningInstance();

    /** Runs the message loop until quit(), and returns the code given to it. Returns at once if quit() was called
        before it. */
    int run();

    /** Stops the loop, so run() returns code once what is queued has run. From any thread. */
    void quit(int code = 0);

    /** Says the program won't quit after all, having been asked through onQuitRequested. On a Mac this stops at once a
        logging out or a shutdown that is waiting to hear, which would otherwise wait until the system gives up on it. */
    void declineQuit();

    /** Whether quit() has been called. */
    bool isQuitting() const noexcept;

    /** Asks the program to quit as the system does, through onQuitRequested, or quits when there is no handler. */
    void requestQuit();

    /** Ends the process at once, with nothing destroyed, as for a helper whose message thread may be held by what it
        is running. */
    [[noreturn]] static void terminate(int code);

    /** Whether the program is in front, its windows the ones with the keyboard. */
    bool isForeground() const;

    /** On a Mac, shows the program in the Dock and the application switcher, or takes it out of them, as a helper
        nobody should see is; elsewhere it does nothing. */
    void setDockIconVisible(bool visible);

    /** Adds a file to the system's list of recent documents: the Dock menu's and Finder's on a Mac, Recent and the
        taskbar's jump list on Windows. */
    void noteRecentFile(const std::filesystem::path& file);

    /** The system asked the program to quit: Quit in the Dock's menu, logging out or shutting down on a Mac, and the
        session ending on Windows. The program asks what it needs to and calls quit(). Without it, quit() is called. */
    std::function<void()> onQuitRequested;

    /** On Windows, the session really is ending: the process ends when this returns, whatever it was doing, so what
        comes after run() may not happen. For saving what must be kept, quickly and without asking anything. */
    std::function<void()> onSessionEnding;

    /** The system asked the program to open files: double-clicked, dropped on its icon, or given to it with Open With.
        On a Mac that includes the file the program was started for; on Windows that comes in arguments(). */
    std::function<void(std::vector<std::filesystem::path> files)> onOpenFiles;

    /** Another instance of the program started and handed over its arguments with sendToRunningInstance. */
    std::function<void(std::vector<std::string> arguments)> onAnotherInstance;

    /** On a Mac, the program's Dock icon was clicked while none of its windows showed. */
    std::function<void()> onReopen;

    struct Impl;

private:
    std::unique_ptr<Impl> impl;
};

}
