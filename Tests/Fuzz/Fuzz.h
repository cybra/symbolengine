#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <string_view>

// What the fuzz targets share. Each defines LLVMFuzzerTestOneInput, called with bytes by libFuzzer where the build has it
// and by Replay.cpp over what is kept in Seeds otherwise, so anything a fuzzer found once stays a test on every system.
namespace fuzz
{

inline std::string_view text(const std::uint8_t* data, std::size_t size)
{
    return {reinterpret_cast<const char*>(data), size};
}

/** A place of this process's own for a file, in a folder taken away when the process ends. */
inline std::filesystem::path scratch(const std::string& name)
{
    static const auto folder = [] {
        std::random_device random;
        auto made = std::filesystem::temp_directory_path() /
                    ("symbolengine-fuzz-" + std::to_string(random()) + "-" + std::to_string(random()));
        std::filesystem::create_directories(made);
        static const struct Remover
        {
            std::filesystem::path path;
            ~Remover()
            {
                std::error_code ignored;
                std::filesystem::remove_all(path, ignored);
            }
        } remover{made};
        return made;
    }();
    return folder / name;
}

inline void write(const std::filesystem::path& file, const std::uint8_t* data, std::size_t size)
{
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
}

/** Something that must always hold didn't, which a fuzzer takes for a crash and keeps the input of. */
[[noreturn]] inline void broken()
{
    std::abort();
}

}
