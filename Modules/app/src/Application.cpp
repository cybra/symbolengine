#include <symbolengine/app/Application.h>

#include "Platform.h"

#include <symbolengine/ui/MessageLoop.h>

#include <atomic>
#include <cassert>

namespace symbolengine
{

namespace
{
Application* made = nullptr;
}

struct Application::Impl
{
    Options options;
    std::vector<std::string> arguments;
    std::atomic<int> quitCode{0};
    std::atomic<bool> quitPosted{false};
};

Application::Application(Options options, int argc, char** argv) : impl(std::make_unique<Impl>())
{
    assert(made == nullptr && "A program makes one Application");
    made = this;

    impl->options = std::move(options);
    if (impl->options.identifier.empty())
        impl->options.identifier = impl->options.name;

    ui::messageLoop::initialize();
    app::platform::start(*this, impl->options.identifier);
    impl->arguments = app::platform::arguments(argc, argv);
}

Application::~Application()
{
    app::platform::finish();
    made = nullptr;
}

Application* Application::current() noexcept
{
    return made;
}

const Application::Options& Application::options() const noexcept
{
    return impl->options;
}

const std::vector<std::string>& Application::arguments() const noexcept
{
    return impl->arguments;
}

bool Application::sendToRunningInstance()
{
    return app::platform::sendToRunningInstance(impl->options.identifier, impl->arguments);
}

int Application::run()
{
    if (!impl->quitPosted)
        ui::messageLoop::run();
    return impl->quitCode;
}

void Application::quit(int code)
{
    impl->quitCode = code;
    impl->quitPosted = true;
    ui::messageLoop::stop();
}

void Application::declineQuit()
{
    app::platform::declineQuit();
}

bool Application::isQuitting() const noexcept
{
    return impl->quitPosted;
}

void Application::requestQuit()
{
    if (onQuitRequested)
        onQuitRequested();
    else
        quit();
}

void Application::terminate(int code)
{
    app::platform::terminate(code);
}

bool Application::isForeground() const
{
    return app::platform::isForeground();
}

void Application::setDockIconVisible(bool visible)
{
    app::platform::setDockIconVisible(visible);
}

void Application::noteRecentFile(const std::filesystem::path& file)
{
    app::platform::noteRecentFile(file);
}

}
