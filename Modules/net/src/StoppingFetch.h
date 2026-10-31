#pragma once

#include "symbolengine/net/Http.h"

#include <functional>

namespace symbolengine::http
{

/** How a system's get() lets a Stopper end it. While this lasts, stopping calls the function given, which ends the system's
    calls and may not wait, on the thread that stops it, or here if it has been stopped already; once this has gone nothing
    calls it, nor is calling it then. */
class StoppingFetch
{
public:
    StoppingFetch(Stopper* stopper, std::function<void()> end);
    ~StoppingFetch();

    StoppingFetch(const StoppingFetch&) = delete;
    StoppingFetch& operator=(const StoppingFetch&) = delete;

    bool stopped() const noexcept;

private:
    Stopper* const stopper;
};

}
