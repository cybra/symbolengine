#pragma once

#include <symbolengine/ui/Window.h>

#include <memory>

/** What a plugin's window does with each system's own views. Main thread only. */
namespace symbolengine::pluginWindowNative
{

/** Readies the program for windows, as a Mac program's shared application. */
void prepare();

/** The view the editor is put in, made for the window and kept for its life: on a Mac an NSView that isn't flipped and
    clips to its bounds, on Windows a plain child window the program never paints over. */
void* makeContainer(ui::Window& window);

/** Lets go of the view once the window has. */
void releaseContainer(void* container);

/** Settles the editor's view once it is in the container: on a Mac the container made a point larger each way and put
    back on the next turn of the run loop, while the window lives. */
void settle(void* container, std::weak_ptr<bool> alive);

/** While it lives, the calling thread's DPI awareness is a plugin window's, per monitor, so the windows it makes and
    sizes are measured as the plugin's are: on Windows; elsewhere it does nothing. */
class Awareness
{
public:
    Awareness();
    ~Awareness();

    Awareness(const Awareness&) = delete;
    Awareness& operator=(const Awareness&) = delete;

private:
    void* previous = nullptr;
};

}
