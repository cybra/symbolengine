#include "Allocations.h"

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>

#if defined(_WIN32)
    #include <malloc.h>
#endif

namespace
{

thread_local bool counting = false;
std::atomic<std::uint64_t> counted{0};

void note() noexcept
{
    if (counting)
        ++counted;
}

void* allocate(std::size_t size)
{
    note();

    if (void* memory = std::malloc(size == 0 ? 1 : size))
        return memory;

    throw std::bad_alloc();
}

void release(void* memory) noexcept
{
    if (memory != nullptr)
        note();

    std::free(memory);
}

void* allocateAligned(std::size_t size, std::align_val_t alignment)
{
    note();
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

void releaseAligned(void* memory) noexcept
{
    if (memory != nullptr)
        note();
#if defined(_WIN32)
    _aligned_free(memory);
#else
    std::free(memory);
#endif
}

}

namespace allocations
{

Counted::Counted()
{
    counting = true;
}

Counted::~Counted()
{
    counting = false;
}

std::uint64_t count()
{
    return counted.load();
}

}

void* operator new(std::size_t size)
{
    return allocate(size);
}

void* operator new[](std::size_t size)
{
    return allocate(size);
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
    try
    {
        return allocate(size);
    }
    catch (...)
    {
        return nullptr;
    }
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
    try
    {
        return allocate(size);
    }
    catch (...)
    {
        return nullptr;
    }
}

void* operator new(std::size_t size, std::align_val_t alignment)
{
    return allocateAligned(size, alignment);
}

void* operator new[](std::size_t size, std::align_val_t alignment)
{
    return allocateAligned(size, alignment);
}

void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
    try
    {
        return allocateAligned(size, alignment);
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

void operator delete(void* memory) noexcept
{
    release(memory);
}

void operator delete[](void* memory) noexcept
{
    release(memory);
}

void operator delete(void* memory, std::size_t) noexcept
{
    release(memory);
}

void operator delete[](void* memory, std::size_t) noexcept
{
    release(memory);
}

void operator delete(void* memory, std::align_val_t) noexcept
{
    releaseAligned(memory);
}

void operator delete[](void* memory, std::align_val_t) noexcept
{
    releaseAligned(memory);
}

void operator delete(void* memory, std::align_val_t, const std::nothrow_t&) noexcept
{
    releaseAligned(memory);
}

void operator delete[](void* memory, std::align_val_t, const std::nothrow_t&) noexcept
{
    releaseAligned(memory);
}

void operator delete(void* memory, std::size_t, std::align_val_t) noexcept
{
    releaseAligned(memory);
}

void operator delete[](void* memory, std::size_t, std::align_val_t) noexcept
{
    releaseAligned(memory);
}
