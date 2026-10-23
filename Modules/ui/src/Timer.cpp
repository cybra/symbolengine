#include <symbolengine/ui/Native.h>

#include <choc/gui/choc_MessageLoop.h>

namespace symbolengine::ui
{

struct Timer::Impl
{
    Impl(std::uint32_t interval, std::function<void()> tick)
        : timer(interval, [tick = std::move(tick)] {
              tick();
              return true;
          })
    {
    }

    choc::messageloop::Timer timer;
};

Timer::Timer(std::uint32_t intervalMilliseconds, std::function<void()> tick)
    : impl(std::make_unique<Impl>(intervalMilliseconds, std::move(tick)))
{
}

Timer::~Timer() = default;

}
