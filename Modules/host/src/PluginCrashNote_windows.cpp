#include "CrashNoteRecords.h"

#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
    #define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace symbolengine::crashNote::native
{

namespace
{

// The file and its mapping, kept for as long as the note is open.
struct Handles
{
    HANDLE file = INVALID_HANDLE_VALUE;
    HANDLE mapping = nullptr;
};

Layout* caught = nullptr;
LPTOP_LEVEL_EXCEPTION_FILTER before = nullptr;

// Text added to the crash's room in the note, with nothing taken from the heap, which the crash may have damaged.
void add(const char* text)
{
    const auto length = std::strlen(text);
    const auto at = std::min<std::size_t>(caught->crashLength, crashRoom);
    const auto taken = std::min(length, crashRoom - at);
    std::memcpy(caught->crash + at, text, taken);
    caught->crashLength = static_cast<std::uint32_t>(at + taken);
}

// A crash no guard of the host's caught, as on a plugin's own thread: the thread marked as the one it came on, its
// exception's code and each frame of its stack as its module and offset written to the note, and then the crash goes
// on to whatever handled it before, the system's own report among them.
LONG WINAPI onCrash(EXCEPTION_POINTERS* exception)
{
    if (caught != nullptr)
    {
        if (const auto index = threadRecordIn(caught); index >= 0)
            caught->threads[index].crashed = 1;

        char line[2048];
        std::snprintf(line, sizeof line, "exception 0x%08lX\n",
                      static_cast<unsigned long>(exception->ExceptionRecord->ExceptionCode));
        add(line);
        void* frames[64];
        const auto count = CaptureStackBackTrace(0, 64, frames, nullptr);

        for (USHORT i = 0; i < count; ++i)
        {
            HMODULE module = nullptr;
            wchar_t wide[1024] = L"?";
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   static_cast<LPCWSTR>(frames[i]), &module))
                GetModuleFileNameW(module, wide, 1024);
            char path[1024 * 3] = {};
            WideCharToMultiByte(CP_UTF8, 0, wide, -1, path, sizeof path, nullptr, nullptr);
            std::snprintf(
                line, sizeof line, "%u %s+0x%llx\n", static_cast<unsigned>(i), path,
                static_cast<unsigned long long>(static_cast<char*>(frames[i]) - reinterpret_cast<char*>(module)));
            add(line);
        }
    }

    return before != nullptr ? before(exception) : EXCEPTION_CONTINUE_SEARCH;
}

}

Layout* map(const std::filesystem::path& file, void*& handle)
{
    auto* handles = new Handles;
    handles->file =
        CreateFileW(file.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (handles->file == INVALID_HANDLE_VALUE)
    {
        delete handles;
        return nullptr;
    }

    LARGE_INTEGER size{};
    size.QuadPart = static_cast<LONGLONG>(sizeof(Layout));
    handles->mapping = CreateFileMappingW(handles->file, nullptr, PAGE_READWRITE, static_cast<DWORD>(size.HighPart),
                                          size.LowPart, nullptr);
    void* memory =
        handles->mapping != nullptr ? MapViewOfFile(handles->mapping, FILE_MAP_WRITE, 0, 0, sizeof(Layout)) : nullptr;

    if (memory == nullptr)
    {
        if (handles->mapping != nullptr)
            CloseHandle(handles->mapping);
        CloseHandle(handles->file);
        delete handles;
        return nullptr;
    }

    handle = handles;
    return static_cast<Layout*>(memory);
}

void unmap(Layout* layout, void* handle)
{
    auto* handles = static_cast<Handles*>(handle);
    UnmapViewOfFile(layout);
    CloseHandle(handles->mapping);
    CloseHandle(handles->file);
    delete handles;
}

void catchCrashes(Layout* layout, void*)
{
    caught = layout;
    before = SetUnhandledExceptionFilter(onCrash);
}

void stopCatching()
{
    SetUnhandledExceptionFilter(before);
    before = nullptr;
    caught = nullptr;
}

}
