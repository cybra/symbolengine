#include "symbolengine/core/MappedFile.h"

#ifndef NOMINMAX
    #define NOMINMAX
#endif
#include <windows.h>

namespace symbolengine
{

std::unique_ptr<MappedFile> MappedFile::open(const std::filesystem::path& file, std::error_code& error)
{
    error.clear();
    const auto lastError = [] { return std::error_code(static_cast<int>(GetLastError()), std::system_category()); };

    // Other programs, as virus scanners and search indexers do, may go on opening it.
    const auto handle = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (handle == INVALID_HANDLE_VALUE)
    {
        error = lastError();
        return nullptr;
    }

    std::unique_ptr<MappedFile> mapped(new MappedFile);
    LARGE_INTEGER size{};

    if (!GetFileSizeEx(handle, &size))
    {
        error = lastError();
        CloseHandle(handle);
        return nullptr;
    }

    if (size.QuadPart > 0)
    {
        const auto mapping = CreateFileMappingW(handle, nullptr, PAGE_READONLY, 0, 0, nullptr);

        if (mapping == nullptr)
        {
            error = lastError();
            CloseHandle(handle);
            return nullptr;
        }

        const auto* start = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);

        if (start == nullptr)
            error = lastError();

        CloseHandle(mapping);

        if (start == nullptr)
        {
            CloseHandle(handle);
            return nullptr;
        }

        mapped->start = start;
        mapped->length = static_cast<std::size_t>(size.QuadPart);
    }

    CloseHandle(handle);
    return mapped;
}

MappedFile::~MappedFile()
{
    if (start != nullptr)
        UnmapViewOfFile(start);
}

}
