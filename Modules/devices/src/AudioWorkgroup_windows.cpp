#include "AudioWorkgroupDetail.h"

#ifndef NOMINMAX
    #define NOMINMAX
#endif
#include <windows.h>

#include <avrt.h>

#include <algorithm>
#include <thread>

namespace symbolengine
{

struct AudioWorkgroup::Impl
{
};

struct AudioWorkgroup::Membership::State
{
    HANDLE task = nullptr;
};

int AudioWorkgroup::maxThreads() const noexcept
{
    return impl == nullptr ? 1 : static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
}

AudioWorkgroup::Membership AudioWorkgroup::join(double, int) const
{
    if (impl == nullptr)
        return {};
    DWORD index = 0;
    auto state = std::make_unique<Membership::State>();
    state->task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &index);
    if (state->task == nullptr)
        return {};
    return Membership(std::move(state));
}

AudioWorkgroup::Membership::Membership(std::unique_ptr<State> s) : state(std::move(s)) {}

AudioWorkgroup::Membership::~Membership()
{
    if (state != nullptr)
        AvRevertMmThreadCharacteristics(state->task);
}

AudioWorkgroup::Membership::Membership(Membership&& other) noexcept = default;

AudioWorkgroup::Membership& AudioWorkgroup::Membership::operator=(Membership&& other) noexcept
{
    if (this != &other)
    {
        if (state != nullptr)
            AvRevertMmThreadCharacteristics(state->task);
        state = std::move(other.state);
    }
    return *this;
}

namespace devices::detail
{
AudioWorkgroup appleWorkgroup(void*)
{
    return {};
}

AudioWorkgroup proAudioWorkgroup()
{
    return AudioWorkgroup(std::make_shared<AudioWorkgroup::Impl>());
}
}

}
