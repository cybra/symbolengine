#include "Scratch.h"

#include <symbolengine/core/Files.h>
#include <symbolengine/core/Sha256.h>

#include <catch2/catch_test_macros.hpp>

#include <random>
#include <string>

using namespace symbolengine;

namespace
{
std::string hexOf(std::string_view bytes)
{
    return Sha256::hex(Sha256::of(bytes));
}
}

TEST_CASE("SHA-256 gives FIPS 180-4's digests")
{
    CHECK(hexOf("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(hexOf("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(hexOf("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    CHECK(
        hexOf("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrst"
              "nopqrstu") == "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");
    CHECK(hexOf(std::string(1000000, 'a')) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("SHA-256 pads every length around a block's end")
{
    CHECK(hexOf(std::string(55, 'a')) == "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
    CHECK(hexOf(std::string(56, 'a')) == "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
    CHECK(hexOf(std::string(63, 'a')) == "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34");
    CHECK(hexOf(std::string(64, 'a')) == "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
    CHECK(hexOf(std::string(65, 'a')) == "635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0");
    CHECK(hexOf(std::string(119, 'a')) == "31eba51c313a5c08226adf18d4a359cfdfd8d2e816b13f4af952f7ea6584dcfb");
    CHECK(hexOf(std::string(120, 'a')) == "2f3d335432c70b580af0e8e1b3674a7c020d683aa5f73aaaedfdc55af904c21c");
    CHECK(hexOf(std::string(128, 'a')) == "6836cf13bac400e9105071cd6af47084dfacad4e5e302c94bfed24e013afb73e");
}

TEST_CASE("SHA-256 given its bytes in pieces of any size gives the digest of them whole, and starts again after")
{
    std::mt19937 random(11);
    std::string bytes(5000, '\0');
    for (auto& byte : bytes)
        byte = static_cast<char>(random());
    const auto whole = Sha256::of(bytes);

    Sha256 hash;
    for (int round = 0; round < 50; ++round)
    {
        for (std::size_t at = 0; at < bytes.size();)
        {
            const auto piece = std::min<std::size_t>(random() % 200, bytes.size() - at);
            hash.update(bytes.data() + at, piece);
            at += piece;
        }
        REQUIRE(hash.finish() == whole);
    }

    hash.update("abc");
    CHECK(Sha256::hex(hash.finish()) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST_CASE("SHA-256 of a file is the digest of its contents, and a file that can't be read says so")
{
    ScratchFolder scratch;
    std::string contents(200000, '\0');
    for (std::size_t i = 0; i < contents.size(); ++i)
        contents[i] = static_cast<char>(i * 31 + 7);
    const auto file = scratch.path / "release.zip";
    files::writeSafely(file, contents);

    CHECK(Sha256::ofFile(file) == Sha256::of(contents));

    std::error_code error;
    CHECK(Sha256::ofFile(scratch.path / "missing.zip", error) == Sha256::Digest{});
    CHECK(error);
    CHECK_THROWS_AS(Sha256::ofFile(scratch.path / "missing.zip"), std::filesystem::filesystem_error);
}
