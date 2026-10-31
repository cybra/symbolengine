#pragma once

// Included in one source file of a program, in its debug build or its tests, this replaces operator new and delete
// so that every allocation and free made inside a device's cycle is counted, as audioThread::allocations() gives:
// a program's audio callbacks, and the devices module's own work around them, should leave it where it was.

#include "AudioThread.h"

#include <cstddef>
#include <cstdlib>
#include <new>

#if defined(_WIN32)
    #include <malloc.h>
#endif

namespace symbolengine::audioThread::detail
{
inline void* allocate(std::size_t size)
{
    if (isCurrent())
        noteAllocation();
    if (void* memory = std::malloc(size == 0 ? 1 : size))
        return memory;
    throw std::bad_alloc();
}

inline void release(void* memory) noexcept
{
    if (memory != nullptr && isCurrent())
        noteAllocation();
    std::free(memory);
}

inline void* allocateAligned(std::size_t size, std::align_val_t alignment)
{
    if (isCurrent())
        noteAllocation();
    const auto align = static_cast<std::size_t>(alignment);
#if defined(_WIN32)
    if (void* memory = _aligned_malloc(size == 0 ? 1 : size, align))
        return memory;
#else
    void* memory = nullptr;
    if (posix_memalign(&memory, align < sizeof(void*) ? sizeof(void*) : align, size == 0 ? 1 : size) == 0)
        return memory;
#endif
    throw std::bad_alloc();
}

inline void releaseAligned(void* memory) noexcept
{
    if (memory != nullptr && isCurrent())
        noteAllocation();
#if defined(_WIN32)
    _aligned_free(memory);
#else
    std::free(memory);
#endif
}
}

// The thread sanitizer on Linux links its own operators into every program whole, so nothing can replace them there, and
// nothing is counted in that build; every other build counts.
#define SYMBOLENGINE_SANITIZER_KEEPS_NEW 0
#if defined(__linux__)
    #if defined(__SANITIZE_THREAD__)
        #undef SYMBOLENGINE_SANITIZER_KEEPS_NEW
        #define SYMBOLENGINE_SANITIZER_KEEPS_NEW 1
    #elif defined(__has_feature)
        #if __has_feature(thread_sanitizer)
            #undef SYMBOLENGINE_SANITIZER_KEEPS_NEW
            #define SYMBOLENGINE_SANITIZER_KEEPS_NEW 1
        #endif
    #endif
#endif

#if !SYMBOLENGINE_SANITIZER_KEEPS_NEW
void* operator new(std::size_t size) { return symbolengine::audioThread::detail::allocate(size); }
void* operator new[](std::size_t size) { return symbolengine::audioThread::detail::allocate(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
    try
    {
        return symbolengine::audioThread::detail::allocate(size);
    }
    catch (...)
    {
        return nullptr;
    }
}
void* operator new[](std::size_t size, const std::nothrow_t& tag) noexcept { return operator new(size, tag); }
void operator delete(void* memory) noexcept { symbolengine::audioThread::detail::release(memory); }
void operator delete[](void* memory) noexcept { symbolengine::audioThread::detail::release(memory); }
void operator delete(void* memory, std::size_t) noexcept { symbolengine::audioThread::detail::release(memory); }
void operator delete[](void* memory, std::size_t) noexcept { symbolengine::audioThread::detail::release(memory); }
void operator delete(void* memory, const std::nothrow_t&) noexcept { symbolengine::audioThread::detail::release(memory); }
void operator delete[](void* memory, const std::nothrow_t&) noexcept
{
    symbolengine::audioThread::detail::release(memory);
}

void* operator new(std::size_t size, std::align_val_t alignment)
{
    return symbolengine::audioThread::detail::allocateAligned(size, alignment);
}
void* operator new[](std::size_t size, std::align_val_t alignment)
{
    return symbolengine::audioThread::detail::allocateAligned(size, alignment);
}
void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
    try
    {
        return symbolengine::audioThread::detail::allocateAligned(size, alignment);
    }
    catch (...)
    {
        return nullptr;
    }
}
void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t& tag) noexcept
{
    return operator new(size, alignment, tag);
}
void operator delete(void* memory, std::align_val_t, const std::nothrow_t&) noexcept
{
    symbolengine::audioThread::detail::releaseAligned(memory);
}
void operator delete[](void* memory, std::align_val_t, const std::nothrow_t&) noexcept
{
    symbolengine::audioThread::detail::releaseAligned(memory);
}
void operator delete(void* memory, std::align_val_t) noexcept
{
    symbolengine::audioThread::detail::releaseAligned(memory);
}
void operator delete[](void* memory, std::align_val_t) noexcept
{
    symbolengine::audioThread::detail::releaseAligned(memory);
}
void operator delete(void* memory, std::size_t, std::align_val_t) noexcept
{
    symbolengine::audioThread::detail::releaseAligned(memory);
}
void operator delete[](void* memory, std::size_t, std::align_val_t) noexcept
{
    symbolengine::audioThread::detail::releaseAligned(memory);
}
#endif
