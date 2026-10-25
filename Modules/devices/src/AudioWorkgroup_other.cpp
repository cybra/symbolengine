#include "AudioWorkgroupDetail.h"

namespace symbolengine
{

struct AudioWorkgroup::Impl
{
};

struct AudioWorkgroup::Membership::State
{
};

int AudioWorkgroup::maxThreads() const noexcept
{
    return 1;
}

AudioWorkgroup::Membership AudioWorkgroup::join(double, int) const
{
    return {};
}

AudioWorkgroup::Membership::Membership(std::unique_ptr<State> s) : state(std::move(s)) {}
AudioWorkgroup::Membership::~Membership() = default;
AudioWorkgroup::Membership::Membership(Membership&& other) noexcept = default;
AudioWorkgroup::Membership& AudioWorkgroup::Membership::operator=(Membership&& other) noexcept = default;

namespace devices::detail
{
AudioWorkgroup appleWorkgroup(void*)
{
    return {};
}

AudioWorkgroup proAudioWorkgroup()
{
    return {};
}
}

}
