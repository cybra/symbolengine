#include "symbolengine/core/MappedFile.h"

#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace symbolengine
{

std::unique_ptr<MappedFile> MappedFile::open(const std::filesystem::path& file, std::error_code& error)
{
    error.clear();
    const auto descriptor = ::open(file.c_str(), O_RDONLY | O_CLOEXEC);

    if (descriptor < 0)
    {
        error = std::error_code(errno, std::generic_category());
        return nullptr;
    }

    std::unique_ptr<MappedFile> mapped(new MappedFile);
    struct stat status{};

    if (fstat(descriptor, &status) != 0)
    {
        error = std::error_code(errno, std::generic_category());
        close(descriptor);
        return nullptr;
    }

    if (status.st_size > 0)
    {
        const auto length = static_cast<std::size_t>(status.st_size);
        const auto* start = mmap(nullptr, length, PROT_READ, MAP_PRIVATE, descriptor, 0);

        if (start == MAP_FAILED)
        {
            error = std::error_code(errno, std::generic_category());
            close(descriptor);
            return nullptr;
        }

        mapped->start = start;
        mapped->length = length;
    }

    close(descriptor);
    return mapped;
}

MappedFile::~MappedFile()
{
    if (start != nullptr)
        munmap(const_cast<void*>(start), length);
}

}
