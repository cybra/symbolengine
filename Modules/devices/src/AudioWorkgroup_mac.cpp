#include "AudioWorkgroupDetail.h"

#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <os/workgroup.h>
#include <pthread.h>

#include <algorithm>
#include <cstdint>

namespace symbolengine
{

struct AudioWorkgroup::Impl
{
    explicit Impl(os_workgroup_t w) : workgroup(w) {}
    ~Impl() { os_release(workgroup); }
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    os_workgroup_t workgroup;
};

struct AudioWorkgroup::Membership::State
{
    std::shared_ptr<AudioWorkgroup::Impl> group;
    os_workgroup_join_token_s token{};
};

int AudioWorkgroup::maxThreads() const noexcept
{
    return impl == nullptr ? 1 : std::max(1, os_workgroup_max_parallel_threads(impl->workgroup, nullptr));
}

AudioWorkgroup::Membership AudioWorkgroup::join(double sampleRate, int blockFrames) const
{
    if (impl == nullptr || sampleRate <= 0.0 || blockFrames <= 0)
        return {};

    // The block's period in the machine's ticks, half of it computing, as the audio thread itself is given.
    mach_timebase_info_data_t timebase{};
    mach_timebase_info(&timebase);
    const auto period = 1e9 * blockFrames / sampleRate * timebase.denom / timebase.numer;
    thread_time_constraint_policy_data_t policy{static_cast<std::uint32_t>(period), static_cast<std::uint32_t>(period / 2),
                                                static_cast<std::uint32_t>(period), 1};
    thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY,
                      reinterpret_cast<thread_policy_t>(&policy), THREAD_TIME_CONSTRAINT_POLICY_COUNT);

    auto state = std::make_unique<Membership::State>();
    state->group = impl;
    if (os_workgroup_join(impl->workgroup, &state->token) != 0)
        return {};
    return Membership(std::move(state));
}

AudioWorkgroup::Membership::Membership(std::unique_ptr<State> s) : state(std::move(s)) {}

AudioWorkgroup::Membership::~Membership()
{
    if (state != nullptr)
        os_workgroup_leave(state->group->workgroup, &state->token);
}

AudioWorkgroup::Membership::Membership(Membership&& other) noexcept = default;

AudioWorkgroup::Membership& AudioWorkgroup::Membership::operator=(Membership&& other) noexcept
{
    if (this != &other)
    {
        if (state != nullptr)
            os_workgroup_leave(state->group->workgroup, &state->token);
        state = std::move(other.state);
    }
    return *this;
}

namespace devices::detail
{
AudioWorkgroup appleWorkgroup(void* retained)
{
    if (retained == nullptr)
        return {};
    return AudioWorkgroup(std::make_shared<AudioWorkgroup::Impl>(static_cast<os_workgroup_t>(retained)));
}

AudioWorkgroup proAudioWorkgroup()
{
    return {};
}
}

}
