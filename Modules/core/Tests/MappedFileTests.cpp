#include "symbolengine/core/Files.h"
#include "symbolengine/core/MappedFile.h"
#include "symbolengine/core/Paths.h"

#include "Scratch.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

using namespace symbolengine;

TEST_CASE("A mapped file holds the file's contents")
{
    ScratchFolder scratch;
    const auto file = scratch.path / "bytes.bin";
    std::string contents(70000, 'a');
    contents[69999] = 'z';
    contents[3] = '\0';
    files::writeSafely(file, contents);

    std::error_code error;
    const auto mapped = MappedFile::open(file, error);
    REQUIRE(mapped != nullptr);
    CHECK_FALSE(error);
    CHECK(mapped->size() == contents.size());
    CHECK(std::string_view(static_cast<const char*>(mapped->data()), mapped->size()) == contents);
}

TEST_CASE("A mapped file opens under a name outside the system's code page")
{
    ScratchFolder scratch;
    const auto file = scratch.path / paths::fromUtf8("caf\xc3\xa9 \xe6\x97\xa5\xe6\x9c\xac.bin");
    files::writeSafely(file, "held");

    std::error_code error;
    const auto mapped = MappedFile::open(file, error);
    REQUIRE(mapped != nullptr);
    CHECK(mapped->size() == 4);
}

TEST_CASE("An empty mapped file holds nothing")
{
    ScratchFolder scratch;
    const auto file = scratch.path / "empty.bin";
    files::writeSafely(file, "");

    std::error_code error;
    const auto mapped = MappedFile::open(file, error);
    REQUIRE(mapped != nullptr);
    CHECK_FALSE(error);
    CHECK(mapped->size() == 0);
    CHECK(mapped->data() == nullptr);
}

TEST_CASE("A file that can't be mapped says why")
{
    ScratchFolder scratch;
    std::error_code error;
    CHECK(MappedFile::open(scratch.path / "missing.bin", error) == nullptr);
    CHECK(error);

    error.clear();
    CHECK(MappedFile::open(scratch.path, error) == nullptr);
    CHECK(error);
}
