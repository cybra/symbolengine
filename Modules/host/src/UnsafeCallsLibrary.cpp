// What a program links, or the system inserts into it as it starts, on a Mac and Linux, to see the calls a plugin makes
// on the audio thread that a real-time thread never should. It stands in for each such function in the whole process,
// the plugins' code and every library's, passing every call on, and tells the host of those a thread makes while the
// host watches it. A key of the thread's, read without allocating or waiting, says that; a thread's own storage might
// do both the first time it is touched, which here is from inside the allocator.
//
// Calls the real function makes on the way are of no interest, as what the plugin called has been told, so the key is
// empty until it returns. So is it while the host is told, so nothing it does is seen.

#include "UnsafeCallsLink.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cstddef>

#if defined(__APPLE__)
    #include <dispatch/dispatch.h>
    #include <libkern/OSAtomic.h>
    #include <mach/mach_time.h>
    #include <mach/semaphore.h>
    #include <malloc/malloc.h>
    #include <os/lock.h>
    #include <sys/event.h>
#else
    #include <malloc.h>
    #include <sys/epoll.h>
#endif

using namespace symbolengine::unsafeCalls;

namespace
{

pthread_key_t key;
bool ready = false;
std::atomic<Saw> listener{nullptr};

// Runs before the program's own code, and before any plugin's. Calls made earlier, as the system starts the process,
// pass straight on.
__attribute__((constructor)) void start()
{
    ready = pthread_key_create(&key, nullptr) == 0;
}

class Seen
{
public:
    Seen(const char* function, int kind, void* returnAddress)
    {
        if (!ready || function == nullptr)
            return;

        watched = pthread_getspecific(key);

        if (watched == nullptr)
            return;

        pthread_setspecific(key, nullptr);

        if (const auto told = listener.load(std::memory_order_relaxed))
            told(watched, function, kind, returnAddress);
    }

    ~Seen()
    {
        if (watched != nullptr)
            pthread_setspecific(key, watched);
    }

    Seen(const Seen&) = delete;
    Seen& operator=(const Seen&) = delete;

private:
    void* watched = nullptr;
};

void listen(Saw saw)
{
    listener.store(saw, std::memory_order_relaxed);
}

void enter(void* watched)
{
    pthread_setspecific(key, watched);
}

void leave()
{
    pthread_setspecific(key, nullptr);
}

void refresh() {}

const Link theLink{listen, enter, leave, refresh};

}

extern "C" __attribute__((visibility("default"))) const Link* symbolengine_unsafe_calls_link()
{
    return ready ? &theLink : nullptr;
}

#define SEEN(kind, name) const Seen seen(#name, kind, __builtin_return_address(0))

#if defined(__APPLE__)

// The system's dynamic linker sends every call of the second to the first, in every library but this one, so the name
// here still means the real function.
struct Interposing
{
    const void* replacement;
    const void* replaced;
};

    #define INTERPOSE(name)                                                                                            \
        __attribute__((used)) static const Interposing interposing_##name                                              \
            __attribute__((section("__DATA,__interpose"))) = {reinterpret_cast<const void*>(watched_##name),           \
                                                              reinterpret_cast<const void*>(name)};

    #define WATCH(kind, returns, name, parameters, arguments)                                                          \
        static returns watched_##name parameters                                                                       \
        {                                                                                                              \
            SEEN(kind, name);                                                                                          \
            return name arguments;                                                                                     \
        }                                                                                                              \
        INTERPOSE(name)

// Typed allocations, which the system's own libraries and its C++ library use; on systems before them, no such function
// comes to be replaced.
    #pragma clang diagnostic ignored "-Wunguarded-availability-new"

extern "C"
{
    using TypeId = unsigned long long;
    __attribute__((weak_import)) void* malloc_type_malloc(size_t, TypeId);
    __attribute__((weak_import)) void* malloc_type_calloc(size_t, size_t, TypeId);
    __attribute__((weak_import)) void* malloc_type_realloc(void*, size_t, TypeId);
    __attribute__((weak_import)) void* malloc_type_valloc(size_t, TypeId);
    __attribute__((weak_import)) void* malloc_type_aligned_alloc(size_t, size_t, TypeId);
    __attribute__((weak_import)) int malloc_type_posix_memalign(void**, size_t, size_t, TypeId);
    __attribute__((weak_import)) void* malloc_type_zone_malloc(malloc_zone_t*, size_t, TypeId);
    __attribute__((weak_import)) void* malloc_type_zone_calloc(malloc_zone_t*, size_t, size_t, TypeId);
    __attribute__((weak_import)) void* malloc_type_zone_realloc(malloc_zone_t*, void*, size_t, TypeId);
    __attribute__((weak_import)) void* malloc_type_zone_valloc(malloc_zone_t*, size_t, TypeId);
    __attribute__((weak_import)) void* malloc_type_zone_memalign(malloc_zone_t*, size_t, size_t, TypeId);
    int objc_sync_enter(void*);
}

WATCH(memoryCall, void*, malloc, (size_t size), (size))
WATCH(memoryCall, void*, calloc, (size_t count, size_t size), (count, size))
WATCH(memoryCall, void*, realloc, (void* memory, size_t size), (memory, size))
WATCH(memoryCall, void*, reallocf, (void* memory, size_t size), (memory, size))
WATCH(memoryCall, void*, valloc, (size_t size), (size))
WATCH(memoryCall, void*, aligned_alloc, (size_t alignment, size_t size), (alignment, size))
WATCH(memoryCall, int, posix_memalign, (void** memory, size_t alignment, size_t size), (memory, alignment, size))
WATCH(memoryCall, void*, malloc_zone_malloc, (malloc_zone_t * zone, size_t size), (zone, size))
WATCH(memoryCall, void*, malloc_zone_calloc, (malloc_zone_t * zone, size_t count, size_t size), (zone, count, size))
WATCH(memoryCall, void*, malloc_zone_realloc, (malloc_zone_t * zone, void* memory, size_t size), (zone, memory, size))
WATCH(memoryCall, void*, malloc_zone_valloc, (malloc_zone_t * zone, size_t size), (zone, size))
WATCH(memoryCall, void*, malloc_zone_memalign, (malloc_zone_t * zone, size_t alignment, size_t size),
      (zone, alignment, size))
WATCH(memoryCall, void*, malloc_type_malloc, (size_t size, TypeId type), (size, type))
WATCH(memoryCall, void*, malloc_type_calloc, (size_t count, size_t size, TypeId type), (count, size, type))
WATCH(memoryCall, void*, malloc_type_realloc, (void* memory, size_t size, TypeId type), (memory, size, type))
WATCH(memoryCall, void*, malloc_type_valloc, (size_t size, TypeId type), (size, type))
WATCH(memoryCall, void*, malloc_type_aligned_alloc, (size_t alignment, size_t size, TypeId type),
      (alignment, size, type))
WATCH(memoryCall, int, malloc_type_posix_memalign, (void** memory, size_t alignment, size_t size, TypeId type),
      (memory, alignment, size, type))
WATCH(memoryCall, void*, malloc_type_zone_malloc, (malloc_zone_t * zone, size_t size, TypeId type), (zone, size, type))
WATCH(memoryCall, void*, malloc_type_zone_calloc, (malloc_zone_t * zone, size_t count, size_t size, TypeId type),
      (zone, count, size, type))
WATCH(memoryCall, void*, malloc_type_zone_realloc, (malloc_zone_t * zone, void* memory, size_t size, TypeId type),
      (zone, memory, size, type))
WATCH(memoryCall, void*, malloc_type_zone_valloc, (malloc_zone_t * zone, size_t size, TypeId type), (zone, size, type))
WATCH(memoryCall, void*, malloc_type_zone_memalign, (malloc_zone_t * zone, size_t alignment, size_t size, TypeId type),
      (zone, alignment, size, type))
WATCH(memoryCall, void*, mmap, (void* at, size_t size, int protection, int flags, int file, off_t offset),
      (at, size, protection, flags, file, offset))
WATCH(memoryCall, int, munmap, (void* at, size_t size), (at, size))

// Giving back nothing costs nothing, as a smart pointer holding none does.
static void watched_free(void* memory)
{
    const Seen seen(memory != nullptr ? "free" : nullptr, memoryCall, __builtin_return_address(0));
    free(memory);
}
INTERPOSE(free)

static void watched_malloc_zone_free(malloc_zone_t* zone, void* memory)
{
    const Seen seen(memory != nullptr ? "malloc_zone_free" : nullptr, memoryCall, __builtin_return_address(0));
    malloc_zone_free(zone, memory);
}
INTERPOSE(malloc_zone_free)

WATCH(lockCall, int, pthread_mutex_lock, (pthread_mutex_t * lock), (lock))
WATCH(lockCall, int, pthread_rwlock_rdlock, (pthread_rwlock_t * lock), (lock))
WATCH(lockCall, int, pthread_rwlock_wrlock, (pthread_rwlock_t * lock), (lock))
WATCH(lockCall, int, pthread_cond_wait, (pthread_cond_t * condition, pthread_mutex_t* lock), (condition, lock))
WATCH(lockCall, int, pthread_cond_timedwait, (pthread_cond_t * condition, pthread_mutex_t* lock, const timespec* until),
      (condition, lock, until))
WATCH(lockCall, int, pthread_cond_signal, (pthread_cond_t * condition), (condition))
WATCH(lockCall, int, pthread_cond_broadcast, (pthread_cond_t * condition), (condition))
WATCH(lockCall, void, os_unfair_lock_lock, (os_unfair_lock_t lock), (lock))
WATCH(lockCall, int, sem_wait, (sem_t * semaphore), (semaphore))
WATCH(lockCall, kern_return_t, semaphore_wait, (semaphore_t semaphore), (semaphore))
WATCH(lockCall, kern_return_t, semaphore_timedwait, (semaphore_t semaphore, mach_timespec_t wait), (semaphore, wait))
WATCH(lockCall, intptr_t, dispatch_semaphore_wait, (dispatch_semaphore_t semaphore, dispatch_time_t until),
      (semaphore, until))
WATCH(lockCall, intptr_t, dispatch_group_wait, (dispatch_group_t group, dispatch_time_t until), (group, until))
WATCH(lockCall, void, dispatch_sync, (dispatch_queue_t queue, dispatch_block_t block), (queue, block))
WATCH(lockCall, int, objc_sync_enter, (void* object), (object))

    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wdeprecated-declarations"
WATCH(lockCall, void, OSSpinLockLock, (volatile OSSpinLock * lock), (lock))
    #pragma clang diagnostic pop

WATCH(sleepCall, unsigned int, sleep, (unsigned int seconds), (seconds))
WATCH(sleepCall, int, usleep, (useconds_t time), (time))
WATCH(sleepCall, int, nanosleep, (const timespec* time, timespec* left), (time, left))
WATCH(sleepCall, kern_return_t, mach_wait_until, (uint64_t until), (until))
WATCH(sleepCall, int, sched_yield, (), ())

WATCH(threadCall, int, pthread_create,
      (pthread_t * thread, const pthread_attr_t* attributes, void* (*run)(void*), void* argument),
      (thread, attributes, run, argument))
WATCH(threadCall, int, pthread_join, (pthread_t thread, void** result), (thread, result))
WATCH(threadCall, void, dispatch_async, (dispatch_queue_t queue, dispatch_block_t block), (queue, block))
WATCH(threadCall, void, dispatch_after, (dispatch_time_t when, dispatch_queue_t queue, dispatch_block_t block),
      (when, queue, block))

static int watched_open(const char* path, int flags, ...)
{
    SEEN(fileCall, open);
    va_list rest;
    va_start(rest, flags);
    const auto mode = static_cast<mode_t>(va_arg(rest, int));
    va_end(rest);
    return open(path, flags, mode);
}
INTERPOSE(open)

static int watched_openat(int folder, const char* path, int flags, ...)
{
    SEEN(fileCall, openat);
    va_list rest;
    va_start(rest, flags);
    const auto mode = static_cast<mode_t>(va_arg(rest, int));
    va_end(rest);
    return openat(folder, path, flags, mode);
}
INTERPOSE(openat)

WATCH(networkCall, int, kevent,
      (int queue, const struct kevent* changes, int changeCount, struct kevent* events, int eventCount,
       const timespec* limit),
      (queue, changes, changeCount, events, eventCount, limit))

#else

// A function of the name the system's own has, which comes first in every lookup as its library comes before the C
// library in the order the program's are loaded, its header's declaration of the same name left alone. The real one is
// found as it is first called.
    #define WATCH(kind, returns, name, parameters, arguments)                                                          \
        extern "C" __attribute__((visibility("default"))) returns watched_##name parameters __asm__(#name);            \
        returns watched_##name parameters                                                                              \
        {                                                                                                              \
            using Real = returns(*) parameters;                                                                        \
            static std::atomic<Real> real{nullptr};                                                                    \
            auto found = real.load(std::memory_order_relaxed);                                                         \
            if (found == nullptr)                                                                                      \
            {                                                                                                          \
                found = reinterpret_cast<Real>(dlsym(RTLD_NEXT, #name));                                               \
                real.store(found, std::memory_order_relaxed);                                                          \
            }                                                                                                          \
            SEEN(kind, name);                                                                                          \
            return found arguments;                                                                                    \
        }

// The C library's own names for its allocator, as looking one up may allocate.
extern "C"
{
    void* __libc_malloc(size_t);
    void* __libc_calloc(size_t, size_t);
    void* __libc_realloc(void*, size_t);
    void* __libc_memalign(size_t, size_t);
    void* __libc_valloc(size_t);
    void* __libc_pvalloc(size_t);
    void __libc_free(void*);
}

    #define WATCH_ALLOCATOR(returns, name, parameters, arguments)                                                      \
        extern "C" __attribute__((visibility("default"))) returns watched_##name parameters __asm__(#name);            \
        returns watched_##name parameters                                                                              \
        {                                                                                                              \
            SEEN(memoryCall, name);                                                                                    \
            return __libc_##name arguments;                                                                            \
        }

WATCH_ALLOCATOR(void*, malloc, (size_t size), (size))
WATCH_ALLOCATOR(void*, calloc, (size_t count, size_t size), (count, size))
WATCH_ALLOCATOR(void*, realloc, (void* memory, size_t size), (memory, size))
WATCH_ALLOCATOR(void*, memalign, (size_t alignment, size_t size), (alignment, size))
WATCH_ALLOCATOR(void*, valloc, (size_t size), (size))
WATCH_ALLOCATOR(void*, pvalloc, (size_t size), (size))

// Giving back nothing costs nothing, as a smart pointer holding none does.
extern "C" __attribute__((visibility("default"))) void watched_free(void* memory) __asm__("free");
void watched_free(void* memory)
{
    const Seen seen(memory != nullptr ? "free" : nullptr, memoryCall, __builtin_return_address(0));
    __libc_free(memory);
}

WATCH(memoryCall, void*, aligned_alloc, (size_t alignment, size_t size), (alignment, size))
WATCH(memoryCall, int, posix_memalign, (void** memory, size_t alignment, size_t size), (memory, alignment, size))
WATCH(memoryCall, void*, reallocarray, (void* memory, size_t count, size_t size), (memory, count, size))
WATCH(memoryCall, void*, mmap, (void* at, size_t size, int protection, int flags, int file, off_t offset),
      (at, size, protection, flags, file, offset))
WATCH(memoryCall, void*, mmap64, (void* at, size_t size, int protection, int flags, int file, off64_t offset),
      (at, size, protection, flags, file, offset))
WATCH(memoryCall, int, munmap, (void* at, size_t size), (at, size))

WATCH(lockCall, int, pthread_mutex_lock, (pthread_mutex_t * lock), (lock))
WATCH(lockCall, int, pthread_rwlock_rdlock, (pthread_rwlock_t * lock), (lock))
WATCH(lockCall, int, pthread_rwlock_wrlock, (pthread_rwlock_t * lock), (lock))
WATCH(lockCall, int, pthread_spin_lock, (pthread_spinlock_t * lock), (lock))
WATCH(lockCall, int, pthread_cond_wait, (pthread_cond_t * condition, pthread_mutex_t* lock), (condition, lock))
WATCH(lockCall, int, pthread_cond_timedwait, (pthread_cond_t * condition, pthread_mutex_t* lock, const timespec* until),
      (condition, lock, until))
WATCH(lockCall, int, pthread_cond_clockwait,
      (pthread_cond_t * condition, pthread_mutex_t* lock, clockid_t clock, const timespec* until),
      (condition, lock, clock, until))
WATCH(lockCall, int, pthread_cond_signal, (pthread_cond_t * condition), (condition))
WATCH(lockCall, int, pthread_cond_broadcast, (pthread_cond_t * condition), (condition))
WATCH(lockCall, int, sem_wait, (sem_t * semaphore), (semaphore))
WATCH(lockCall, int, sem_timedwait, (sem_t * semaphore, const timespec* until), (semaphore, until))

WATCH(sleepCall, unsigned int, sleep, (unsigned int seconds), (seconds))
WATCH(sleepCall, int, usleep, (useconds_t time), (time))
WATCH(sleepCall, int, nanosleep, (const timespec* time, timespec* left), (time, left))
WATCH(sleepCall, int, clock_nanosleep, (clockid_t clock, int flags, const timespec* time, timespec* left),
      (clock, flags, time, left))
WATCH(sleepCall, int, sched_yield, (), ())

WATCH(threadCall, int, pthread_create,
      (pthread_t * thread, const pthread_attr_t* attributes, void* (*run)(void*), void* argument),
      (thread, attributes, run, argument))
WATCH(threadCall, int, pthread_join, (pthread_t thread, void** result), (thread, result))

// Open's last argument is there only for what it creates, but passing what the call came with does no harm.
    #define WATCH_OPEN(name)                                                                                           \
        extern "C" __attribute__((visibility("default"))) int watched_##name(const char* path, int flags,              \
                                                                             ...) __asm__(#name);                      \
        int watched_##name(const char* path, int flags, ...)                                                           \
        {                                                                                                              \
            using Real = int (*)(const char*, int, ...);                                                               \
            static std::atomic<Real> real{nullptr};                                                                    \
            auto found = real.load(std::memory_order_relaxed);                                                         \
            if (found == nullptr)                                                                                      \
            {                                                                                                          \
                found = reinterpret_cast<Real>(dlsym(RTLD_NEXT, #name));                                               \
                real.store(found, std::memory_order_relaxed);                                                          \
            }                                                                                                          \
            SEEN(fileCall, name);                                                                                      \
            va_list rest;                                                                                              \
            va_start(rest, flags);                                                                                     \
            const auto mode = static_cast<mode_t>(va_arg(rest, int));                                                  \
            va_end(rest);                                                                                              \
            return found(path, flags, mode);                                                                           \
        }

WATCH_OPEN(open)
WATCH_OPEN(open64)

extern "C" __attribute__((visibility("default"))) int watched_openat(int folder, const char* path, int flags,
                                                                     ...) __asm__("openat");
int watched_openat(int folder, const char* path, int flags, ...)
{
    using Real = int (*)(int, const char*, int, ...);
    static std::atomic<Real> real{nullptr};
    auto found = real.load(std::memory_order_relaxed);

    if (found == nullptr)
    {
        found = reinterpret_cast<Real>(dlsym(RTLD_NEXT, "openat"));
        real.store(found, std::memory_order_relaxed);
    }

    SEEN(fileCall, openat);
    va_list rest;
    va_start(rest, flags);
    const auto mode = static_cast<mode_t>(va_arg(rest, int));
    va_end(rest);
    return found(folder, path, flags, mode);
}

WATCH(networkCall, int, epoll_wait, (int queue, epoll_event* events, int eventCount, int limit),
      (queue, events, eventCount, limit))

#endif

WATCH(fileCall, FILE*, fopen, (const char* path, const char* mode), (path, mode))
WATCH(fileCall, int, fclose, (FILE * file), (file))
WATCH(fileCall, size_t, fread, (void* to, size_t size, size_t count, FILE* file), (to, size, count, file))
WATCH(fileCall, size_t, fwrite, (const void* from, size_t size, size_t count, FILE* file), (from, size, count, file))
WATCH(fileCall, int, fflush, (FILE * file), (file))
WATCH(fileCall, int, close, (int file), (file))
WATCH(fileCall, ssize_t, read, (int file, void* to, size_t size), (file, to, size))
WATCH(fileCall, ssize_t, write, (int file, const void* from, size_t size), (file, from, size))
WATCH(fileCall, ssize_t, pread, (int file, void* to, size_t size, off_t at), (file, to, size, at))
WATCH(fileCall, ssize_t, pwrite, (int file, const void* from, size_t size, off_t at), (file, from, size, at))
WATCH(fileCall, int, stat, (const char* path, struct stat* to), (path, to))
WATCH(fileCall, int, lstat, (const char* path, struct stat* to), (path, to))
WATCH(fileCall, int, fsync, (int file), (file))
WATCH(fileCall, int, access, (const char* path, int mode), (path, mode))

WATCH(networkCall, int, socket, (int domain, int type, int protocol), (domain, type, protocol))
WATCH(networkCall, int, connect, (int socket, const sockaddr* to, socklen_t size), (socket, to, size))
WATCH(networkCall, int, accept, (int socket, sockaddr* from, socklen_t* size), (socket, from, size))
WATCH(networkCall, ssize_t, send, (int socket, const void* from, size_t size, int flags), (socket, from, size, flags))
WATCH(networkCall, ssize_t, recv, (int socket, void* to, size_t size, int flags), (socket, to, size, flags))
WATCH(networkCall, ssize_t, sendto,
      (int socket, const void* from, size_t size, int flags, const sockaddr* to, socklen_t toSize),
      (socket, from, size, flags, to, toSize))
WATCH(networkCall, ssize_t, recvfrom,
      (int socket, void* to, size_t size, int flags, sockaddr* from, socklen_t* fromSize),
      (socket, to, size, flags, from, fromSize))
WATCH(networkCall, int, poll, (pollfd * files, nfds_t count, int limit), (files, count, limit))
WATCH(networkCall, int, select, (int count, fd_set* reads, fd_set* writes, fd_set* errors, timeval* limit),
      (count, reads, writes, errors, limit))
