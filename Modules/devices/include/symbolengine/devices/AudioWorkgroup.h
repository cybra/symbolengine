#pragma once

#include <memory>

namespace symbolengine
{

/** What a thread of the program's own joins when the audio thread waits for its work, as plugins processed on
    several threads are, so the system schedules it to the device's deadline: on a Mac the device's audio workgroup,
    which Apple Silicon needs for work spread over threads to finish in time, and on Windows the multimedia
    scheduler's Pro Audio class. Empty where a system gives none, and joining an empty one does nothing. */
class AudioWorkgroup
{
public:
    AudioWorkgroup() = default;

    bool empty() const noexcept { return impl == nullptr; }

    /** How many threads the system suggests doing the work at once, the audio thread among them; 1 when it says
        nothing. */
    int maxThreads() const noexcept;

    /** The calling thread's place in the workgroup, until it goes, on the thread that joined. */
    class Membership
    {
    public:
        Membership() = default;
        ~Membership();
        Membership(Membership&& other) noexcept;
        Membership& operator=(Membership&& other) noexcept;
        Membership(const Membership&) = delete;
        Membership& operator=(const Membership&) = delete;

        bool joined() const noexcept { return state != nullptr; }

        struct State;
        explicit Membership(std::unique_ptr<State> s);

    private:
        std::unique_ptr<State> state;
    };

    /** Joins the calling thread, first given the real-time period of the device's blocks, as a Mac asks before a
        thread may join; not joined when the system refuses, as for a workgroup the device has let go of. */
    Membership join(double sampleRate, int blockFrames) const;

    struct Impl;
    explicit AudioWorkgroup(std::shared_ptr<Impl> i) : impl(std::move(i)) {}

private:
    std::shared_ptr<Impl> impl;
};

}
