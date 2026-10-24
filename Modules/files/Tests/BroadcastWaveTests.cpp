#include "Pattern.h"
#include "Scratch.h"

#include <symbolengine/files/BroadcastWave.h>

#include <catch2/catch_test_macros.hpp>

using namespace symbolengine;

TEST_CASE("A broadcast extension written by another program is read field by field")
{
    const auto reader = files::openAudio(pattern::reference("bwf-pcm24-stereo-48000.wav"));
    const auto extension = files::broadcastExtension(reader->info().chunks);
    REQUIRE(extension.has_value());
    CHECK(extension->description == "A take for the tests");
    CHECK(extension->originator == "SymbolEngine");
    CHECK(extension->originatorReference == "REF-0001");
    CHECK(extension->originationDate == "2026-10-23");
    CHECK(extension->originationTime == "21:45:30");
    CHECK(extension->timeReference == 172800000);
    CHECK(extension->codingHistory == "A=PCM,F=48000,W=24,M=stereo");
    CHECK(extension->umid.empty());
}

TEST_CASE("A broadcast extension written into a WAV reads back as it was, longer fields cut at their size")
{
    ScratchFolder folder;
    const auto file = folder.path / "take.wav";

    BroadcastExtension extension;
    extension.description = "Recorded in a test";
    extension.originator = "An originator longer than its thirty-two bytes";
    extension.originatorReference = "Take 7";
    extension.originationDate = "2026-10-23";
    extension.originationTime = "23-59-59";
    extension.timeReference = 0x1'0000'0001;   // past 32 bits, as the evening's samples at 96 kHz are
    extension.umid = std::string(32, '\x06');
    extension.codingHistory = "A=PCM,F=48000,W=24,M=mono,T=SymbolEngine\r\n";

    const auto chunk = files::bextChunk(extension);
    CHECK(chunk.id == "bext");
    CHECK(chunk.bytes.size() == 602 + extension.codingHistory.size());

    const float sample = 0.0f;
    const float* channels[1]{&sample};
    {
        auto writer = files::createAudio(file, {"WAV", 48000, 1, 24, false, 0, {chunk}});
        writer->write(channels, 1);
    }

    const auto back = files::broadcastExtension(files::openAudio(file)->info().chunks);
    REQUIRE(back.has_value());
    auto expected = extension;
    expected.originator = extension.originator.substr(0, 32);
    expected.umid = extension.umid + std::string(32, '\0');
    CHECK(*back == expected);
}

TEST_CASE("A version 2 extension's loudness is written as not measured, and a file without one has none")
{
    BroadcastExtension extension;
    extension.version = 2;
    const auto bytes = files::bextChunk(extension).bytes;
    for (std::size_t at = 412; at < 422; at += 2)
        CHECK((static_cast<unsigned char>(bytes[at]) == 0xFF && static_cast<unsigned char>(bytes[at + 1]) == 0x7F));
    CHECK(files::bextChunk(BroadcastExtension{}).bytes.substr(412, 10) == std::string(10, '\0'));

    CHECK(!files::broadcastExtension(files::openAudio(pattern::reference("pcm16-stereo-44100.wav"))->info().chunks).has_value());
    CHECK(!files::broadcastExtension({{"bext", "too short"}}).has_value());
}
