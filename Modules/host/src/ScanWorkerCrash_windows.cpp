#include "ScanWorkerCrash.h"

#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
    #define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <cstring>

namespace symbolengine::scanWorkerCrash
{

namespace
{

HANDLE noteFile = INVALID_HANDLE_VALUE;

void put(const char* text)
{
    DWORD written = 0;
    WriteFile(noteFile, text, static_cast<DWORD>(std::strlen(text)), &written, nullptr);
}

// The exception's code and each frame of the stack as its module and offset, written with nothing taken from the heap,
// which the crash may have damaged; then the system ends the process as it would have, with the exception's code.
LONG WINAPI handle(EXCEPTION_POINTERS* exception)
{
    if (noteFile != INVALID_HANDLE_VALUE)
    {
        SetFilePointer(noteFile, 0, nullptr, FILE_END);
        char line[2048];
        std::snprintf(line, sizeof line, "\nexception 0x%08lX\n",
                      static_cast<unsigned long>(exception->ExceptionRecord->ExceptionCode));
        put(line);
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
            put(line);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

}

void begin(const std::filesystem::path& note)
{
    noteFile = CreateFileW(note.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (noteFile != INVALID_HANDLE_VALUE)
        SetUnhandledExceptionFilter(handle);
}

void note(std::string_view text)
{
    if (noteFile == INVALID_HANDLE_VALUE)
        return;
    DWORD written = 0;
    SetFilePointer(noteFile, 0, nullptr, FILE_BEGIN);
    WriteFile(noteFile, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    SetEndOfFile(noteFile);
}

}
