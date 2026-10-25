#pragma once

namespace symbolengine::audioThread::detail
{

/** Marks the calling thread as inside a device's cycle for as long as it lives. */
class Marker
{
public:
    Marker() noexcept;
    ~Marker();

    Marker(const Marker&) = delete;
    Marker& operator=(const Marker&) = delete;

private:
    bool before;
};

}
