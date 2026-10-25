#include <symbolengine/ui/MessageLoop.h>

#include <choc/gui/choc_MessageLoop.h>

namespace symbolengine::ui::messageLoop
{

void initialize()
{
    choc::messageloop::initialise();
}

void run()
{
    choc::messageloop::run();
}

void stop()
{
    choc::messageloop::stop();
}

bool isMessageThread()
{
    return choc::messageloop::callerIsOnMessageThread();
}

void callAsync(std::function<void()> function)
{
    choc::messageloop::postMessage(std::move(function));
}

void callAfter(std::uint32_t milliseconds, std::function<void()> function)
{
    choc::messageloop::setTimeout(milliseconds, std::move(function));
}

}
