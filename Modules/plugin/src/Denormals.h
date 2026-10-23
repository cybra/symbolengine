#pragma once

#include <cstdint>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    #include <xmmintrin.h>
#endif

namespace symbolengine::plugin::detail
{

/** Flushes denormal numbers to zero while it lives. A filter or reverb decaying towards silence passes through them,
    and on x86 each one can cost a hundred times an ordinary number, enough to overload the audio thread. */
class NoDenormals
{
public:
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    // Flush to zero for results, and denormals are zero for inputs.
    NoDenormals() noexcept : previous(_mm_getcsr()) { _mm_setcsr(previous | 0x8040); }
    ~NoDenormals() { _mm_setcsr(previous); }

private:
    unsigned int previous;
#elif defined(__aarch64__)
    NoDenormals() noexcept
    {
        asm volatile("mrs %0, fpcr" : "=r"(previous));
        asm volatile("msr fpcr, %0" : : "r"(previous | (std::uint64_t{1} << 24)));   // flush to zero
    }
    ~NoDenormals() { asm volatile("msr fpcr, %0" : : "r"(previous)); }

private:
    std::uint64_t previous;
#else
    NoDenormals() noexcept {}
#endif

public:
    NoDenormals(const NoDenormals&) = delete;
    NoDenormals& operator=(const NoDenormals&) = delete;
};

}
