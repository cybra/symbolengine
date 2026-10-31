// On Windows every module calls another's functions through its own table of imports, which the system fills in as it
// loads it, and nothing else need be linked or inserted: each entry for a function of interest, in every module in the
// process, the plugins' and the system's own, is pointed here, where a thread's own value says whether the host is
// watching, and the call goes on to the function itself. So what a plugin calls and what the C library calls for it
// are both seen, a plugin with the C library built in among them. What a module looks up by name as it runs goes
// straight to the function and isn't seen, nor is a library loaded since the host last looked, which it does as each
// plugin is prepared.

#include "UnsafeCalls.h"

#ifndef NOMINMAX
    #define NOMINMAX
#endif
#include <windows.h>

#include <objbase.h>
#include <tlhelp32.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(_MSC_VER)
    #include <intrin.h>
    #define SYMBOLENGINE_RETURN_ADDRESS() _ReturnAddress()
#else
    #define SYMBOLENGINE_RETURN_ADDRESS() __builtin_return_address(0)
#endif

namespace symbolengine::unsafeCalls
{

namespace
{

// A thread's own, and read in these calls only once the system has made it.
thread_local void* watchedHere = nullptr;
std::atomic<Saw> listener{nullptr};

class Seen
{
public:
    Seen(const char* function, int kind, void* returnAddress)
    {
        if (function == nullptr)
            return;

        watched = watchedHere;

        if (watched == nullptr)
            return;

        watchedHere = nullptr;

        if (const auto told = listener.load(std::memory_order_relaxed))
            told(watched, function, kind, returnAddress);
    }

    ~Seen()
    {
        if (watched != nullptr)
            watchedHere = watched;
    }

    Seen(const Seen&) = delete;
    Seen& operator=(const Seen&) = delete;

private:
    void* watched = nullptr;
};

struct Watched
{
    const char* name;
    void* replacement;
    void** real;
};

#define WATCH(kind, returns, convention, name, parameters, arguments)                                                  \
    returns(convention* real_##name) parameters = nullptr;                                                             \
    returns convention watched_##name parameters                                                                       \
    {                                                                                                                  \
        const Seen seen(#name, kind, SYMBOLENGINE_RETURN_ADDRESS());                                                   \
        return real_##name arguments;                                                                                  \
    }

// Giving back nothing costs nothing, as a smart pointer holding none does.
#define WATCH_RELEASE(returns, convention, name, parameters, arguments, memory)                                        \
    returns(convention* real_##name) parameters = nullptr;                                                             \
    returns convention watched_##name parameters                                                                       \
    {                                                                                                                  \
        const Seen seen(memory != nullptr ? #name : nullptr, memoryCall, SYMBOLENGINE_RETURN_ADDRESS());               \
        return real_##name arguments;                                                                                  \
    }

WATCH(memoryCall, LPVOID, WINAPI, HeapAlloc, (HANDLE heap, DWORD flags, SIZE_T size), (heap, flags, size))
WATCH(memoryCall, LPVOID, WINAPI, HeapReAlloc, (HANDLE heap, DWORD flags, LPVOID memory, SIZE_T size),
      (heap, flags, memory, size))
WATCH_RELEASE(BOOL, WINAPI, HeapFree, (HANDLE heap, DWORD flags, LPVOID memory), (heap, flags, memory), memory)
WATCH(memoryCall, PVOID, NTAPI, RtlAllocateHeap, (PVOID heap, ULONG flags, SIZE_T size), (heap, flags, size))
WATCH(memoryCall, PVOID, NTAPI, RtlReAllocateHeap, (PVOID heap, ULONG flags, PVOID memory, SIZE_T size),
      (heap, flags, memory, size))
WATCH_RELEASE(BOOLEAN, NTAPI, RtlFreeHeap, (PVOID heap, ULONG flags, PVOID memory), (heap, flags, memory), memory)
WATCH(memoryCall, LPVOID, WINAPI, VirtualAlloc, (LPVOID at, SIZE_T size, DWORD type, DWORD protection),
      (at, size, type, protection))
WATCH(memoryCall, BOOL, WINAPI, VirtualFree, (LPVOID at, SIZE_T size, DWORD type), (at, size, type))
WATCH(memoryCall, HLOCAL, WINAPI, LocalAlloc, (UINT flags, SIZE_T size), (flags, size))
WATCH_RELEASE(HLOCAL, WINAPI, LocalFree, (HLOCAL memory), (memory), memory)
WATCH(memoryCall, HGLOBAL, WINAPI, GlobalAlloc, (UINT flags, SIZE_T size), (flags, size))
WATCH_RELEASE(HGLOBAL, WINAPI, GlobalFree, (HGLOBAL memory), (memory), memory)
WATCH(memoryCall, LPVOID, WINAPI, CoTaskMemAlloc, (SIZE_T size), (size))
WATCH(memoryCall, LPVOID, WINAPI, CoTaskMemRealloc, (LPVOID memory, SIZE_T size), (memory, size))
WATCH_RELEASE(void, WINAPI, CoTaskMemFree, (LPVOID memory), (memory), memory)

WATCH(lockCall, void, WINAPI, EnterCriticalSection, (LPCRITICAL_SECTION section), (section))
WATCH(lockCall, LONG, NTAPI, RtlEnterCriticalSection, (PRTL_CRITICAL_SECTION section), (section))
WATCH(lockCall, void, WINAPI, AcquireSRWLockExclusive, (PSRWLOCK lock), (lock))
WATCH(lockCall, void, WINAPI, AcquireSRWLockShared, (PSRWLOCK lock), (lock))
WATCH(lockCall, void, NTAPI, RtlAcquireSRWLockExclusive, (PSRWLOCK lock), (lock))
WATCH(lockCall, void, NTAPI, RtlAcquireSRWLockShared, (PSRWLOCK lock), (lock))
WATCH(lockCall, DWORD, WINAPI, WaitForSingleObject, (HANDLE object, DWORD limit), (object, limit))
WATCH(lockCall, DWORD, WINAPI, WaitForSingleObjectEx, (HANDLE object, DWORD limit, BOOL alertable),
      (object, limit, alertable))
WATCH(lockCall, DWORD, WINAPI, WaitForMultipleObjects, (DWORD count, const HANDLE* objects, BOOL all, DWORD limit),
      (count, objects, all, limit))
WATCH(lockCall, DWORD, WINAPI, WaitForMultipleObjectsEx,
      (DWORD count, const HANDLE* objects, BOOL all, DWORD limit, BOOL alertable),
      (count, objects, all, limit, alertable))
WATCH(lockCall, DWORD, WINAPI, SignalObjectAndWait, (HANDLE signal, HANDLE object, DWORD limit, BOOL alertable),
      (signal, object, limit, alertable))
WATCH(lockCall, BOOL, WINAPI, SleepConditionVariableCS,
      (PCONDITION_VARIABLE condition, PCRITICAL_SECTION section, DWORD limit), (condition, section, limit))
WATCH(lockCall, BOOL, WINAPI, SleepConditionVariableSRW,
      (PCONDITION_VARIABLE condition, PSRWLOCK lock, DWORD limit, ULONG flags), (condition, lock, limit, flags))
WATCH(lockCall, void, WINAPI, WakeConditionVariable, (PCONDITION_VARIABLE condition), (condition))
WATCH(lockCall, void, WINAPI, WakeAllConditionVariable, (PCONDITION_VARIABLE condition), (condition))

WATCH(sleepCall, void, WINAPI, Sleep, (DWORD time), (time))
WATCH(sleepCall, DWORD, WINAPI, SleepEx, (DWORD time, BOOL alertable), (time, alertable))
WATCH(sleepCall, BOOL, WINAPI, SwitchToThread, (), ())

WATCH(fileCall, HANDLE, WINAPI, CreateFileW,
      (LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security, DWORD disposition, DWORD flags,
       HANDLE from),
      (path, access, share, security, disposition, flags, from))
WATCH(fileCall, HANDLE, WINAPI, CreateFileA,
      (LPCSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security, DWORD disposition, DWORD flags,
       HANDLE from),
      (path, access, share, security, disposition, flags, from))
WATCH(fileCall, BOOL, WINAPI, ReadFile, (HANDLE file, LPVOID to, DWORD size, LPDWORD read, LPOVERLAPPED overlapped),
      (file, to, size, read, overlapped))
WATCH(fileCall, BOOL, WINAPI, WriteFile,
      (HANDLE file, LPCVOID from, DWORD size, LPDWORD written, LPOVERLAPPED overlapped),
      (file, from, size, written, overlapped))
WATCH(fileCall, BOOL, WINAPI, FlushFileBuffers, (HANDLE file), (file))
WATCH(fileCall, DWORD, WINAPI, GetFileAttributesW, (LPCWSTR path), (path))

WATCH(threadCall, HANDLE, WINAPI, CreateThread,
      (LPSECURITY_ATTRIBUTES security, SIZE_T stack, LPTHREAD_START_ROUTINE start, LPVOID argument, DWORD flags,
       LPDWORD id),
      (security, stack, start, argument, flags, id))
WATCH(threadCall, BOOL, WINAPI, QueueUserWorkItem, (LPTHREAD_START_ROUTINE start, PVOID argument, ULONG flags),
      (start, argument, flags))

#define WATCHED(name) {#name, reinterpret_cast<void*>(&watched_##name), reinterpret_cast<void**>(&real_##name)}

const Watched everything[] = {
    WATCHED(HeapAlloc),
    WATCHED(HeapReAlloc),
    WATCHED(HeapFree),
    WATCHED(RtlAllocateHeap),
    WATCHED(RtlReAllocateHeap),
    WATCHED(RtlFreeHeap),
    WATCHED(VirtualAlloc),
    WATCHED(VirtualFree),
    WATCHED(LocalAlloc),
    WATCHED(LocalFree),
    WATCHED(GlobalAlloc),
    WATCHED(GlobalFree),
    WATCHED(CoTaskMemAlloc),
    WATCHED(CoTaskMemRealloc),
    WATCHED(CoTaskMemFree),
    WATCHED(EnterCriticalSection),
    WATCHED(RtlEnterCriticalSection),
    WATCHED(AcquireSRWLockExclusive),
    WATCHED(AcquireSRWLockShared),
    WATCHED(RtlAcquireSRWLockExclusive),
    WATCHED(RtlAcquireSRWLockShared),
    WATCHED(WaitForSingleObject),
    WATCHED(WaitForSingleObjectEx),
    WATCHED(WaitForMultipleObjects),
    WATCHED(WaitForMultipleObjectsEx),
    WATCHED(SignalObjectAndWait),
    WATCHED(SleepConditionVariableCS),
    WATCHED(SleepConditionVariableSRW),
    WATCHED(WakeConditionVariable),
    WATCHED(WakeAllConditionVariable),
    WATCHED(Sleep),
    WATCHED(SleepEx),
    WATCHED(SwitchToThread),
    WATCHED(CreateFileW),
    WATCHED(CreateFileA),
    WATCHED(ReadFile),
    WATCHED(WriteFile),
    WATCHED(FlushFileBuffers),
    WATCHED(GetFileAttributesW),
    WATCHED(CreateThread),
    WATCHED(QueueUserWorkItem),
};

// The function itself, as far from what imports it as can be found, as kernel32's own entry for some of these only goes
// on through its own table, which is pointed here too. Found in what is loaded, as what imports it has its library
// loaded already.
void* functionItself(const char* name)
{
    for (const auto* const library : {L"ntdll.dll", L"kernelbase.dll", L"kernel32.dll", L"combase.dll", L"ole32.dll"})
        if (const auto module = GetModuleHandleW(library))
            if (const auto found = GetProcAddress(module, name))
                return reinterpret_cast<void*>(found);

    return nullptr;
}

// Whether the library an import comes from is the system's, and not another that names a function the same.
bool fromTheSystem(const char* library)
{
    if (_strnicmp(library, "api-ms-", 7) == 0 || _strnicmp(library, "ext-ms-", 7) == 0)
        return true;

    for (const auto* const name : {"ntdll.dll", "kernelbase.dll", "kernel32.dll", "combase.dll", "ole32.dll"})
        if (_stricmp(library, name) == 0)
            return true;

    return false;
}

void pointHere(void** entry, void* replacement)
{
    if (*entry == replacement)
        return;

    DWORD before = 0;

    if (VirtualProtect(entry, sizeof(void*), PAGE_READWRITE, &before) == 0)
        return;

    // One word, which a thread calling through it at this moment reads whole, either way going to the same function.
    std::atomic_ref<void*>(*entry).store(replacement);
    VirtualProtect(entry, sizeof(void*), before, &before);
}

void takeIn(HMODULE module)
{
    auto* const base = reinterpret_cast<BYTE*>(module);
    const auto* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);

    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return;

    const auto* const headers = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);

    if (headers->Signature != IMAGE_NT_SIGNATURE)
        return;

    const auto& imports = headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];

    if (imports.VirtualAddress == 0)
        return;

    for (auto* from = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + imports.VirtualAddress); from->Name != 0;
         ++from)
    {
        if (from->OriginalFirstThunk == 0 || !fromTheSystem(reinterpret_cast<const char*>(base + from->Name)))
            continue;

        const auto* names = reinterpret_cast<const IMAGE_THUNK_DATA*>(base + from->OriginalFirstThunk);
        auto** entries = reinterpret_cast<void**>(base + from->FirstThunk);

        for (; names->u1.AddressOfData != 0; ++names, ++entries)
        {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
                continue;

            const auto* const named = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            const auto* const name = reinterpret_cast<const char*>(named->Name);

            for (const auto& watched : everything)
            {
                if (std::strcmp(watched.name, name) != 0)
                    continue;

                if (*watched.real == nullptr)
                    *watched.real = functionItself(name);

                if (*watched.real != nullptr)
                    pointHere(entries, watched.replacement);

                break;
            }
        }
    }
}

void listen(Saw saw)
{
    listener.store(saw, std::memory_order_relaxed);
}

void enter(void* watched)
{
    watchedHere = watched;
}

void leave()
{
    watchedHere = nullptr;
}

void refresh()
{
    const auto modules = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());

    if (modules == INVALID_HANDLE_VALUE)
        return;

    MODULEENTRY32W entry{};
    entry.dwSize = sizeof entry;

    for (auto more = Module32FirstW(modules, &entry); more; more = Module32NextW(modules, &entry))
        takeIn(entry.hModule);

    CloseHandle(modules);
}

const Link theLink{listen, enter, leave, refresh};

std::string utf8(const std::wstring& wide)
{
    const auto size =
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string text(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), text.data(), size, nullptr, nullptr);
    return text;
}

}

const Link* link()
{
    return &theLink;
}

void tryCalls()
{
    void* volatile memory = std::malloc(16);
    std::free(memory);
    SRWLOCK lock = SRWLOCK_INIT;
    AcquireSRWLockExclusive(&lock);
    ReleaseSRWLockExclusive(&lock);
}

int captureFrames(void** frames, int most) noexcept
{
    return CaptureStackBackTrace(0, static_cast<DWORD>(most), frames, nullptr);
}

Frame describeFrame(void* frame)
{
    HMODULE module = nullptr;

    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCWSTR>(frame), &module) == 0)
        return {{}, "?"};

    std::wstring path(32768, L'\0');
    path.resize(GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size())));
    char offset[32];
    std::snprintf(offset, sizeof offset, "+0x%llx",
                  static_cast<unsigned long long>(static_cast<char*>(frame) - reinterpret_cast<char*>(module)));
    return {path, utf8(path) + offset};
}

bool isSystemModule(const std::filesystem::path& module)
{
    wchar_t folder[MAX_PATH];
    const auto size = GetSystemWindowsDirectoryW(folder, MAX_PATH);
    return size > 0 && size < MAX_PATH && _wcsnicmp(module.c_str(), folder, size) == 0;
}

std::filesystem::path programModule()
{
    std::wstring path(32768, L'\0');
    path.resize(GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size())));
    return path;
}

}
