#include "ProcessState.h"

#include <clocale>
#include <system_error>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    #include <xmmintrin.h>
    #define SYMBOLENGINE_X86 1
#endif

namespace symbolengine
{

namespace
{

#if defined(SYMBOLENGINE_X86)
// MXCSR's modes: denormals as zero, the exception masks, rounding and flush to zero; below them are the flags that
// arithmetic sets as it goes, which aren't modes.
constexpr std::uint32_t modeBits = 0xFFC0;
#endif

}

std::uint64_t floatingPointModes() noexcept
{
#if defined(SYMBOLENGINE_X86)
    // The x87 unit's control word too, beside SSE's: a plugin setting rounding sets both, and C's own fegetround reads
    // the x87 one where the compiler lets it be reached.
    std::uint64_t x87 = 0;
    #if defined(__GNUC__) || defined(__clang__)
    std::uint16_t word = 0;
    __asm__ volatile("fnstcw %0" : "=m"(word));
    x87 = word;
    #endif
    return (_mm_getcsr() & modeBits) | (x87 << 32);
#elif defined(__aarch64__)
    std::uint64_t control = 0;
    __asm__ volatile("mrs %0, fpcr" : "=r"(control));
    return control;
#else
    return 0;
#endif
}

void setFloatingPointModes([[maybe_unused]] std::uint64_t modes) noexcept
{
#if defined(SYMBOLENGINE_X86)
    _mm_setcsr((_mm_getcsr() & ~modeBits) | (static_cast<std::uint32_t>(modes) & modeBits));
    #if defined(__GNUC__) || defined(__clang__)
    const auto word = static_cast<std::uint16_t>(modes >> 32);
    __asm__ volatile("fldcw %0" : : "m"(word));
    #endif
#elif defined(__aarch64__)
    __asm__ volatile("msr fpcr, %0" : : "r"(modes));
#endif
}

ProcessState ProcessState::now()
{
    ProcessState state;
    state.floatingPoint = floatingPointModes();

    if (const char* locale = std::setlocale(LC_ALL, nullptr))
        state.locale = locale;

    std::error_code ignored;
    state.folder = std::filesystem::current_path(ignored);
    state.coalescesMouse = mouseCoalescing();
    return state;
}

bool ProcessState::putBack() const
{
    bool changed = false;

    if (floatingPointModes() != floatingPoint)
    {
        setFloatingPointModes(floatingPoint);
        changed = true;
    }

    if (const char* current = std::setlocale(LC_ALL, nullptr);
        !locale.empty() && (current == nullptr || locale != current))
    {
        std::setlocale(LC_ALL, locale.c_str());
        changed = true;
    }

    std::error_code ignored;

    if (!folder.empty() && std::filesystem::current_path(ignored) != folder)
    {
        std::filesystem::current_path(folder, ignored);
        changed = true;
    }

    if (mouseCoalescing() != coalescesMouse)
    {
        setMouseCoalescing(coalescesMouse);
        changed = true;
    }

    return changed;
}

#if !defined(__APPLE__)
bool mouseCoalescing()
{
    return true;
}

void setMouseCoalescing(bool) {}
#endif

}
