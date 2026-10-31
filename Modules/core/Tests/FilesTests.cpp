#include "Scratch.h"

#include <symbolengine/core/Files.h>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>

    #include <chrono>
    #include <thread>
#else
    #include <sys/stat.h>
    #include <sys/xattr.h>
    #include <unistd.h>
#endif

using namespace symbolengine;

namespace
{
std::vector<std::string> namesIn(const std::filesystem::path& folder)
{
    std::vector<std::string> names;
    for (const auto& entry : std::filesystem::directory_iterator(folder))
        names.push_back(entry.path().filename().string());
    return names;
}

std::string everyByte()
{
    std::string bytes;
    for (int i = 0; i < 256; ++i)
        bytes += static_cast<char>(i);
    return bytes;
}
}

TEST_CASE("A file reads back byte for byte what was written")
{
    ScratchFolder scratch;
    const auto file = scratch.path / "bytes.bin";

    files::writeSafely(file, everyByte());
    CHECK(files::read(file) == everyByte());

    std::string large(5'000'000, '\0');
    for (std::size_t i = 0; i < large.size(); ++i)
        large[i] = static_cast<char>(i * 7919 % 251);
    files::writeSafely(file, large);
    CHECK(files::read(file) == large);

    files::writeSafely(file, "");
    CHECK(files::read(file).empty());
}

TEST_CASE("Writing replaces the file and leaves nothing beside it")
{
    ScratchFolder scratch;
    const auto file = scratch.path / "settings.json";

    files::writeSafely(file, "first");
    files::writeSafely(file, "second, longer than the first");
    files::writeSafely(file, "third");

    CHECK(files::read(file) == "third");
    CHECK(namesIn(scratch.path) == std::vector<std::string>{"settings.json"});
}

TEST_CASE("Writing makes the folders on the way")
{
    ScratchFolder scratch;
    const auto file = scratch.path / "a" / "b" / "c.txt";
    files::writeSafely(file, "deep");
    CHECK(files::read(file) == "deep");
}

TEST_CASE("Reading a missing file says so")
{
    ScratchFolder scratch;
    const auto file = scratch.path / "missing.txt";

    std::error_code error;
    CHECK(files::read(file, error).empty());
    CHECK(error == std::errc::no_such_file_or_directory);

    try
    {
        files::read(file);
        FAIL("read should have thrown");
    }
    catch (const std::filesystem::filesystem_error& thrown)
    {
        CHECK(thrown.path1() == file);
        CHECK(thrown.code() == std::errc::no_such_file_or_directory);
    }
}

TEST_CASE("Writing under a file instead of a folder fails without throwing when asked")
{
    ScratchFolder scratch;
    files::writeSafely(scratch.path / "plain", "x");

    std::error_code error;
    files::writeSafely(scratch.path / "plain" / "inside.txt", "y", error);
    CHECK(error);
    CHECK_THROWS_AS(files::writeSafely(scratch.path / "plain" / "inside.txt", "y"), std::filesystem::filesystem_error);
}

#if !defined(_WIN32)
TEST_CASE("Writing keeps the file's permissions")
{
    ScratchFolder scratch;
    const auto file = scratch.path / "private.txt";

    files::writeSafely(file, "secret");
    ::chmod(file.c_str(), 0600);
    files::writeSafely(file, "new secret");

    struct stat status = {};
    REQUIRE(::stat(file.c_str(), &status) == 0);
    CHECK((status.st_mode & 0777) == 0600);
}

TEST_CASE("Writing through a symbolic link replaces the file it points to")
{
    ScratchFolder scratch;
    const auto target = scratch.path / "real.txt";
    const auto link = scratch.path / "link.txt";

    files::writeSafely(target, "old");
    std::filesystem::create_symlink("real.txt", link);
    files::writeSafely(link, "new");

    CHECK(std::filesystem::is_symlink(link));
    CHECK(files::read(target) == "new");
}

TEST_CASE("A write that fails leaves the old contents and no new file")
{
    if (::geteuid() == 0)
        SKIP("permissions don't stop the superuser");

    ScratchFolder scratch;
    const auto folder = scratch.path / "locked";
    const auto file = folder / "kept.txt";

    files::writeSafely(file, "old");
    std::filesystem::permissions(folder, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec);

    std::error_code error;
    files::writeSafely(file, "new", error);
    CHECK(error == std::errc::permission_denied);

    std::filesystem::permissions(folder, std::filesystem::perms::owner_all);
    CHECK(files::read(file) == "old");
    CHECK(namesIn(folder) == std::vector<std::string>{"kept.txt"});
}
#endif

#if defined(_WIN32)
TEST_CASE("Writing waits for a program holding the file open for a moment")
{
    ScratchFolder scratch;
    const auto file = scratch.path / "held.txt";
    files::writeSafely(file, "old");

    // Held open with no sharing at all, as some virus scanners do, and let go after 300 ms.
    HANDLE held = CreateFileW(file.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(held != INVALID_HANDLE_VALUE);

    std::thread release([held] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        CloseHandle(held);
    });

    std::error_code error;
    files::writeSafely(file, "new", error);
    release.join();

    CHECK_FALSE(error);
    CHECK(files::read(file) == "new");
    CHECK(namesIn(scratch.path) == std::vector<std::string>{"held.txt"});
}
#endif

#if !defined(_WIN32)
TEST_CASE("A file named as long as a name can be is written like any other")
{
    ScratchFolder scratch;
    const auto file = scratch.path / std::string(250, 'a');
    files::writeSafely(file, "one");
    files::writeSafely(file, "two");
    CHECK(files::read(file) == "two");
}
#endif

#if defined(__APPLE__) || defined(__linux__)
TEST_CASE("Writing keeps what the system holds about a file besides its contents")
{
    ScratchFolder scratch;
    const auto file = scratch.path / "tagged.txt";
    files::writeSafely(file, "one");

    const char value[] = "kept";
    #if defined(__APPLE__)
    const char* const name = "com.example.symbolengine";
    const auto set = ::setxattr(file.c_str(), name, value, sizeof value, 0, 0);
    #else
    const char* const name = "user.symbolengine";
    const auto set = ::setxattr(file.c_str(), name, value, sizeof value, 0);
    #endif
    if (set != 0)
        SKIP("This folder keeps no extended attributes");

    files::writeSafely(file, "two");
    CHECK(files::read(file) == "two");

    char back[16]{};
    #if defined(__APPLE__)
    const auto got = ::getxattr(file.c_str(), name, back, sizeof back, 0, 0);
    #else
    const auto got = ::getxattr(file.c_str(), name, back, sizeof back);
    #endif
    CHECK(static_cast<std::size_t>(got) == sizeof value);
    CHECK(std::string(back) == "kept");
}
#endif

#if defined(_WIN32)
TEST_CASE("Writing keeps a file's attributes")
{
    ScratchFolder scratch;
    const auto file = scratch.path / "hidden.txt";
    files::writeSafely(file, "one");
    REQUIRE(SetFileAttributesW(file.c_str(), FILE_ATTRIBUTE_HIDDEN));
    files::writeSafely(file, "two");
    CHECK(files::read(file) == "two");
    const auto attributes = GetFileAttributesW(file.c_str());
    CHECK((attributes & FILE_ATTRIBUTE_HIDDEN) != 0);
    SetFileAttributesW(file.c_str(), FILE_ATTRIBUTE_NORMAL);
}
#endif
