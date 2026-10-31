#include "Scratch.h"

#include <symbolengine/core/Files.h>
#include <symbolengine/core/Paths.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <string>

using namespace symbolengine;
using paths::Folder;

namespace
{
#if defined(_WIN32)
bool isUnder(const std::filesystem::path& path, const std::filesystem::path& folder)
{
    const auto relative = path.lexically_relative(folder);
    return !relative.empty() && *relative.begin() != "..";
}
#endif

#if defined(__linux__)
// Sets an environment variable for one test and puts it back afterwards.
struct Environment
{
    Environment(const char* variableName, const std::string& value) : name(variableName)
    {
        if (const char* old = std::getenv(name))
            previous = old;
        had = std::getenv(name) != nullptr;
        setenv(name, value.c_str(), 1);
    }

    ~Environment()
    {
        if (had)
            setenv(name, previous.c_str(), 1);
        else
            unsetenv(name);
    }

    const char* name;
    std::string previous;
    bool had = false;
};
#endif
}

TEST_CASE("Paths go to and from UTF-8 unchanged")
{
    const std::string name = "M\xC3\xBAsica/\xE6\x97\xA5\xE6\x9C\xAC/\xF0\x9F\x8E\xA7 take.wav";
    CHECK(paths::toUtf8(paths::fromUtf8(name)) == name);
    CHECK(paths::fromUtf8(name).filename() == paths::fromUtf8("\xF0\x9F\x8E\xA7 take.wav"));
}

TEST_CASE("A file named outside ASCII is made under exactly that name")
{
    ScratchFolder scratch;
    const std::string name = "M\xC3\xBAsica \xF0\x9F\x8E\xA7.txt";
    files::writeSafely(scratch.path / paths::fromUtf8(name), "x");

    std::vector<std::string> names;
    for (const auto& entry : std::filesystem::directory_iterator(scratch.path))
        names.push_back(paths::toUtf8(entry.path().filename()));

    REQUIRE(names.size() == 1);
    CHECK(names[0] == name);
}

TEST_CASE("Every system folder is an absolute path, and home and temp exist")
{
    for (const auto folder : {Folder::home, Folder::documents, Folder::music, Folder::desktop, Folder::downloads,
                              Folder::appData, Folder::localAppData, Folder::cache, Folder::temp})
    {
        std::error_code error;
        const auto path = paths::systemFolder(folder, error);
        INFO(static_cast<int>(folder) << ": " << paths::toUtf8(path));
        CHECK_FALSE(error);
        CHECK(path.is_absolute());
    }

    CHECK(std::filesystem::is_directory(paths::systemFolder(Folder::home)));
    CHECK(std::filesystem::is_directory(paths::systemFolder(Folder::temp)));
}

#if defined(__APPLE__)
TEST_CASE("A Mac's folders are the ones in its Library")
{
    const auto home = paths::systemFolder(Folder::home);
    CHECK(paths::systemFolder(Folder::appData) == home / "Library/Application Support");
    CHECK(paths::systemFolder(Folder::localAppData) == home / "Library/Application Support");
    CHECK(paths::systemFolder(Folder::cache) == home / "Library/Caches");
    CHECK(paths::systemFolder(Folder::music) == home / "Music");
    CHECK(paths::systemFolder(Folder::documents) == home / "Documents");
}
#endif

#if defined(_WIN32)
TEST_CASE("Windows' folders are the known folders under the profile")
{
    const auto home = paths::systemFolder(Folder::home);
    CHECK(isUnder(paths::systemFolder(Folder::appData), home));
    CHECK(paths::systemFolder(Folder::appData).filename() == "Roaming");
    CHECK(paths::systemFolder(Folder::localAppData).filename() == "Local");
    CHECK(paths::systemFolder(Folder::cache) == paths::systemFolder(Folder::localAppData));
}
#endif

#if defined(__linux__)
TEST_CASE("Linux follows the XDG variables and user-dirs.dirs")
{
    ScratchFolder scratch;
    const auto home = paths::systemFolder(Folder::home);

    {
        Environment config("XDG_CONFIG_HOME", "relative/is/ignored");
        CHECK(paths::systemFolder(Folder::appData) == home / ".config");
    }

    Environment config("XDG_CONFIG_HOME", scratch.path.string());
    Environment data("XDG_DATA_HOME", (scratch.path / "data").string());
    CHECK(paths::systemFolder(Folder::appData) == scratch.path);
    CHECK(paths::systemFolder(Folder::localAppData) == scratch.path / "data");

    files::writeSafely(scratch.path / "user-dirs.dirs", "# written by xdg-user-dirs-update\n"
                                                        "XDG_MUSIC_DIR=\"$HOME/Tunes\"\n"
                                                        "XDG_DESKTOP_DIR=\"/elsewhere/Desk top\"\n"
                                                        "XDG_DOWNLOAD_DIR=\"$HOME/\"\n");

    CHECK(paths::systemFolder(Folder::music) == home / "Tunes");
    CHECK(paths::systemFolder(Folder::desktop) == "/elsewhere/Desk top");
    CHECK(paths::systemFolder(Folder::downloads) == home);
    CHECK(paths::systemFolder(Folder::documents) == home / "Documents");
}
#endif

TEST_CASE("The executable file is this test program")
{
    const auto file = paths::executableFile();
    CHECK(std::filesystem::is_regular_file(file));
    CHECK(paths::toUtf8(file.filename()).starts_with("symbolengine_core_tests"));
}

TEST_CASE("Text that isn't valid in its encoding becomes a replacement, never an exception")
{
    // Bytes that are no UTF-8, on every system.
    CHECK_NOTHROW(paths::fromUtf8("a\xFF"
                                  "b"));
    CHECK_NOTHROW(paths::toUtf8(paths::fromUtf8("a\xFF"
                                                "b")));

#if defined(_WIN32)
    // A name NTFS allows that no UTF-8 can hold, half of a surrogate pair.
    const std::filesystem::path half(std::wstring(L"a\xD800"
                                                  L"b"));
    CHECK(paths::toUtf8(half) == "a\xEF\xBF\xBD"
                                 "b");
    CHECK(paths::fromUtf8("a\xFF"
                          "b")
              .wstring() == L"a\xFFFD"
                            L"b");
#endif
}
