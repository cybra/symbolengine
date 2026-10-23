#pragma once

#include <filesystem>
#include <random>
#include <string>

// A new, empty folder for one test, removed with everything in it when the test ends.
struct ScratchFolder
{
    ScratchFolder()
    {
        std::random_device random;
        path = std::filesystem::temp_directory_path() /
               ("symbolengine-test-" + std::to_string(random()) + "-" + std::to_string(random()));
        std::filesystem::create_directories(path);
    }

    ~ScratchFolder()
    {
        std::error_code ignored;
        std::filesystem::permissions(path, std::filesystem::perms::owner_all, std::filesystem::perm_options::add,
                                     ignored);
        std::filesystem::remove_all(path, ignored);
    }

    ScratchFolder(const ScratchFolder&) = delete;
    ScratchFolder& operator=(const ScratchFolder&) = delete;

    std::filesystem::path path;
};
