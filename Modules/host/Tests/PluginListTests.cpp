#include "Scratch.h"

#include <symbolengine/core/Files.h>
#include <symbolengine/core/Paths.h>
#include <symbolengine/core/System.h>
#include <symbolengine/core/Text.h>
#include <symbolengine/host/PluginList.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <clocale>
#include <cstdint>
#include <fstream>
#include <limits>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <vector>

using namespace symbolengine;
using namespace std::chrono_literals;

namespace
{

using Clock = std::chrono::system_clock;

Clock::time_point at(std::int64_t milliseconds)
{
    return Clock::time_point(std::chrono::duration_cast<Clock::duration>(std::chrono::milliseconds(milliseconds)));
}

// A list in a scratch folder, on a clock the test moves, on a computer that started when the test says.
struct Rig
{
    ScratchFolder scratch;
    std::filesystem::path file = scratch.path / "Plugins arm64.json";
    std::filesystem::path backup = scratch.path / "Plugins arm64.json.backup";
    Clock::time_point time = at(1'761'700'000'000);
    Clock::time_point started = at(1'761'600'000'000);
    std::vector<std::string> logged;

    std::unique_ptr<PluginList> open()
    {
        return std::make_unique<PluginList>(
            PluginList::Options{file, "2.4.1", [this](const std::string& line) { logged.push_back(line); },
                                [this] { return time; }, [this] { return started; }});
    }

    std::filesystem::path place(std::string_view relative) const
    {
        return scratch.path / "Plugins" / paths::fromUtf8(relative);
    }
};

PluginStamp stamp(std::int64_t modified, std::string version = "1.0.0", std::int64_t size = 4096)
{
    return {size, modified, std::move(version), 0};
}

PluginDescription effect(std::string id, std::string name, std::string version = "1.0.0")
{
    PluginDescription plugin;
    plugin.identity = {"VST3", std::move(id)};
    plugin.name = std::move(name);
    plugin.maker = "Test Maker";
    plugin.version = std::move(version);
    plugin.layouts = {{2, 2}, {1, 2}};
    plugin.editor = true;
    plugin.sdk = "VST 3.8.0";
    return plugin;
}

// A VST3 file built for this program's architecture, holding the plugins, each located at it.
PluginFile found(const std::filesystem::path& path, PluginStamp fileStamp, std::vector<PluginDescription> plugins)
{
    PluginFile file;
    file.path = path;
    file.format = "VST3";
    file.stamp = std::move(fileStamp);
    file.architectures = {std::string(system::architecture())};
    for (auto& plugin : plugins)
        plugin.location = path;
    file.plugins = std::move(plugins);
    return file;
}

PluginFile setAside(const std::filesystem::path& path, PluginStamp fileStamp, PluginFailure::Kind kind,
                    std::string step, std::int64_t code)
{
    PluginFile file = found(path, std::move(fileStamp), {});
    file.outcome = PluginFile::Outcome::setAside;
    file.failure = PluginFailure{kind, std::move(step), code, "frame 0: crashed here"};
    return file;
}

const std::string gainId = "5A1B2C3D4E5F60718293A4B5C6D7E8F9";
const std::string delayId = "0123456789ABCDEF0123456789ABCDEF";

std::uint32_t code(const char (&four)[5])
{
    return (std::uint32_t(std::uint8_t(four[0])) << 24) | (std::uint32_t(std::uint8_t(four[1])) << 16) |
           (std::uint32_t(std::uint8_t(four[2])) << 8) | std::uint32_t(std::uint8_t(four[3]));
}

// A host keying plugins by a 32-bit hash of the class ID's four words, multiplying by 31 as it goes.
std::uint32_t shortHash(const std::string& classId)
{
    std::uint32_t hash = 0;
    for (int word = 0; word < 4; ++word)
        hash =
            hash * 31u + static_cast<std::uint32_t>(std::stoul(classId.substr(std::size_t(word) * 8, 8), nullptr, 16));
    return hash;
}

// Writes a file as a damaged one would be left, straight to the disk without the care a list's own saves take, which
// the tests that damage it thousands of times would wait on.
void writePlainly(const std::filesystem::path& file, const std::string& contents)
{
    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    stream << contents;
}

std::vector<std::string> lines(const std::string& text)
{
    std::vector<std::string> pieces;
    for (const auto piece : text::split(text, "\n"))
        pieces.emplace_back(piece);
    return pieces;
}

}

TEST_CASE("Plugins whose shorter hashes collide are two entries, each known by its full identity")
{
    // The second ID's third word is one more and its fourth 31 less, which a 31-multiplier hash can't tell apart.
    const std::string first = "11111111222222223333333344444444";
    const std::string second = "11111111222222223333333444444425";
    REQUIRE(shortHash(first) == shortHash(second));

    // Audio Units whose three codes XORed together are the same.
    const auto unitA = PluginIdentity::audioUnit(code("aufx"), code("Spkr"), code("SyEn"));
    const auto unitB = PluginIdentity::audioUnit(code("aufx"), code("Spks"), code("SyEo"));
    REQUIRE((code("aufx") ^ code("Spkr") ^ code("SyEn")) == (code("aufx") ^ code("Spks") ^ code("SyEo")));

    Rig rig;
    auto list = rig.open();
    list->record(found(rig.place("VST3/Pair.vst3"), stamp(1), {effect(first, "First"), effect(second, "Second")}));
    auto units = found(rig.place("Components/Pair.component"), stamp(2), {});
    units.format = "AudioUnit";
    units.plugins = {effect("", "Unit A"), effect("", "Unit B")};
    units.plugins[0].identity = unitA;
    units.plugins[1].identity = unitB;
    list->record(units);

    REQUIRE(list->plugins().size() == 4);
    CHECK(list->find({"VST3", first})->plugin.name == "First");
    CHECK(list->find({"VST3", second})->plugin.name == "Second");
    CHECK(list->find(unitA)->plugin.name == "Unit A");
    CHECK(list->find(unitB)->plugin.name == "Unit B");
}

TEST_CASE("An Audio Unit's codes are kept exactly, spaces, zeros and bytes past ASCII among them")
{
    CHECK(PluginIdentity::audioUnit(code("aufx"), code("Spkr"), code("SyEn")) ==
          PluginIdentity{"AudioUnit", "61756678-53706b72-5379456e"});
    const auto odd = PluginIdentity::audioUnit(code("aufx"), 0x20200000u, 0xE9416200u);
    CHECK(odd.id == "61756678-20200000-e9416200");
    CHECK(PluginIdentity::audioUnit(0, 0, 0).id == "00000000-00000000-00000000");

    Rig rig;
    {
        auto list = rig.open();
        auto file = found(rig.place("Components/Odd.component"), stamp(1), {effect("", "Odd")});
        file.format = "AudioUnit";
        file.plugins[0].identity = odd;
        list->record(file);
        list->save();
    }
    auto again = rig.open();
    REQUIRE(again->find(odd));
    CHECK(again->find(odd)->plugin.identity == odd);
}

TEST_CASE("Names and makers are kept as they are given")
{
    const std::vector<std::string> names{"Filter: Low [Mono]",  "",
                                         std::string(300, 'n'), "\xC3\x89qualiseur \xE2\x88\x91",
                                         "  spaced  ",          "quote \" and \\ slash"};
    Rig rig;
    std::vector<PluginEntry> before;
    {
        auto list = rig.open();
        std::vector<PluginDescription> plugins;
        for (std::size_t i = 0; i < names.size(); ++i)
        {
            auto plugin = effect(std::string(31, '0') + std::to_string(i), names[i]);
            plugin.maker = "Maker " + names[i];
            plugins.push_back(plugin);
        }
        list->record(found(rig.place("VST3/Names.vst3"), stamp(1), plugins));
        before = list->plugins();
        list->save();
    }
    const auto after = rig.open()->plugins();
    REQUIRE(after.size() == names.size());
    CHECK(after == before);
    for (std::size_t i = 0; i < names.size(); ++i)
        CHECK(after[i].plugin.name == names[i]);
}

TEST_CASE("A plugin moved to another folder keeps its identity and when it was first found")
{
    Rig rig;
    auto list = rig.open();
    list->record(found(rig.place("VST3/Gain.vst3"), stamp(100), {effect(gainId, "Gain")}));
    const auto first = list->find({"VST3", gainId});
    REQUIRE(first);
    CHECK(first->firstSeen == rig.time);

    // A day later it is gone from the system's folder and found in the user's, the file itself unchanged.
    rig.time += 24h;
    list->folderRead(rig.place("VST3"), {});
    CHECK_FALSE(list->find({"VST3", gainId}));
    auto moved = found(rig.place("Mine/Gain.vst3"), stamp(100), {effect(gainId, "Gain")});
    moved.userFolder = true;
    list->record(moved);

    const auto entry = list->find({"VST3", gainId});
    REQUIRE(entry);
    CHECK(entry->plugin.location == rig.place("Mine/Gain.vst3"));
    CHECK(entry->firstSeen == first->firstSeen);
    CHECK(entry->updated == first->updated);
}

TEST_CASE("One plugin found in two places is one entry, loaded by a fixed rule")
{
    const auto native = std::string(system::architecture());
    const std::string other = native == "arm64" ? "x86_64" : "arm64";

    SECTION("The newer version first, whichever folder it is in")
    {
        Rig rig;
        auto list = rig.open();
        list->record(found(rig.place("System/Gain.vst3"), stamp(1, "1.10.0"), {effect(gainId, "Gain", "1.10.0")}));
        auto mine = found(rig.place("Mine/Gain.vst3"), stamp(2, "1.9.3"), {effect(gainId, "Gain", "1.9.3")});
        mine.userFolder = true;
        list->record(mine);

        REQUIRE(list->plugins().size() == 1);
        const auto entry = *list->find({"VST3", gainId});
        CHECK(entry.plugin.location == rig.place("System/Gain.vst3"));
        CHECK(entry.plugin.version == "1.10.0");
        REQUIRE(entry.alternates.size() == 1);
        CHECK(entry.alternates[0].location == rig.place("Mine/Gain.vst3"));
    }

    SECTION("At the same version, the user's folder first, though its path comes later")
    {
        Rig rig;
        auto list = rig.open();
        list->record(found(rig.place("Library/Gain.vst3"), stamp(1), {effect(gainId, "Gain")}));
        auto mine = found(rig.place("User/Gain.vst3"), stamp(1), {effect(gainId, "Gain")});
        mine.userFolder = true;
        list->record(mine);
        CHECK(list->find({"VST3", gainId})->plugin.location == rig.place("User/Gain.vst3"));
    }

    SECTION("A plugin a file lists twice is one place")
    {
        Rig rig;
        auto list = rig.open();
        list->record(found(rig.place("VST3/Gain.vst3"), stamp(1), {effect(gainId, "Gain"), effect(gainId, "Gain")}));
        CHECK(list->file(rig.place("VST3/Gain.vst3"))->plugins.size() == 1);
        CHECK(list->find({"VST3", gainId})->alternates.empty());
    }

    SECTION("A binary for the program's own architecture before a newer one for another")
    {
        Rig rig;
        auto list = rig.open();
        auto newer = found(rig.place("A/Gain.vst3"), stamp(1, "2.0"), {effect(gainId, "Gain", "2.0")});
        newer.architectures = {other};
        list->record(newer);
        list->record(found(rig.place("B/Gain.vst3"), stamp(1, "1.0"), {effect(gainId, "Gain", "1.0")}));
        CHECK(list->find({"VST3", gainId})->plugin.location == rig.place("B/Gain.vst3"));
    }

    SECTION("A place that can be reached before everything else")
    {
        Rig rig;
        auto list = rig.open();
        list->record(found(rig.place("Drive/Gain.vst3"), stamp(1, "3.0"), {effect(gainId, "Gain", "3.0")}));
        list->record(found(rig.place("Local/Gain.vst3"), stamp(1, "1.0"), {effect(gainId, "Gain", "1.0")}));
        list->folderUnreadable(rig.place("Drive"));

        auto entry = *list->find({"VST3", gainId});
        CHECK(entry.plugin.location == rig.place("Local/Gain.vst3"));
        CHECK(entry.connected);

        list->folderUnreadable(rig.place("Local"));
        entry = *list->find({"VST3", gainId});
        CHECK(entry.plugin.location == rig.place("Drive/Gain.vst3"));
        CHECK_FALSE(entry.connected);
    }

    SECTION("Otherwise by path, the same however they were found")
    {
        Rig rig;
        auto list = rig.open();
        list->record(found(rig.place("Z/Gain.vst3"), stamp(1), {effect(gainId, "Gain")}));
        list->record(found(rig.place("A/Gain.vst3"), stamp(1), {effect(gainId, "Gain")}));
        list->record(found(rig.place("M/Gain.vst3"), stamp(1), {effect(gainId, "Gain")}));
        const auto entry = *list->find({"VST3", gainId});
        CHECK(entry.plugin.location == rig.place("A/Gain.vst3"));
        REQUIRE(entry.alternates.size() == 2);
        CHECK(entry.alternates[0].location == rig.place("M/Gain.vst3"));
        CHECK(entry.alternates[1].location == rig.place("Z/Gain.vst3"));
    }
}

TEST_CASE("A file set aside is scanned again when its binary or version changes, and not otherwise")
{
    Rig rig;
    auto list = rig.open();
    const auto path = rig.place("VST3/Crash.vst3");
    const auto before = stamp(1'000, "1.2.0", 50'000);
    CHECK(list->needsScan(path, before));

    list->record(setAside(path, before, PluginFailure::Kind::crashed, "making plugin 1 of 1", 11));
    CHECK_FALSE(list->needsScan(path, before));

    // Touching the bundle leaves the binary inside as it was, and its stamp with it.
    CHECK_FALSE(list->needsScan(path, stamp(1'000, "1.2.0", 50'000)));

    CHECK(list->needsScan(path, stamp(2'000, "1.2.0", 50'000)));   // the binary replaced
    CHECK(list->needsScan(path, stamp(500, "1.2.0", 50'000)));     // an older build put back
    CHECK(list->needsScan(path, stamp(1'000, "1.2.1", 50'000)));   // only its version
    CHECK(list->needsScan(path, stamp(1'000, "1.2.0", 50'001)));   // only its size
    auto manifest = before;
    manifest.manifestModified = 7;
    CHECK(list->needsScan(path, manifest));

    CHECK(list->needsScan(rig.place("VST3/Other.vst3"), before));
}

TEST_CASE("A plugin set aside keeps why, with the date and the program's version and architecture")
{
    Rig rig;
    auto list = rig.open();
    const auto path = rig.place("VST3/Bundle.vst3");
    auto file = found(path, stamp(1), {effect(gainId, "Gain")});
    file.setAside.push_back({effect(delayId, "Delay"), {PluginFailure::Kind::timedOut, "making plugin 2 of 2", 30000}});
    list->record(file);

    const auto kept = list->file(path);
    REQUIRE(kept);
    REQUIRE(kept->setAside.size() == 1);
    const auto& failure = kept->setAside[0].failure;
    CHECK(failure.kind == PluginFailure::Kind::timedOut);
    CHECK(failure.step == "making plugin 2 of 2");
    CHECK(failure.code == 30000);
    CHECK(failure.date == rig.time);
    CHECK(failure.programVersion == "2.4.1");
    CHECK(failure.architecture == system::architecture());
    CHECK(kept->setAside[0].plugin.identity == PluginIdentity{"VST3", delayId});

    // Only the plugin found is listed to be loaded; the one set aside isn't, and the file isn't scanned again.
    CHECK(list->find({"VST3", gainId}));
    CHECK_FALSE(list->find({"VST3", delayId}));
    CHECK_FALSE(list->needsScan(path, stamp(1)));

    const auto crash = rig.place("VST3/Crash.vst3");
    list->record(setAside(crash, stamp(1), PluginFailure::Kind::crashed, "loading the code", 11));
    REQUIRE(list->file(crash)->failure);
    CHECK(list->file(crash)->failure->date == rig.time);
    CHECK(list->file(crash)->failure->detail == "frame 0: crashed here");
}

TEST_CASE("Try Again scans a file again, and a scan cut short is finished at the next")
{
    Rig rig;
    auto list = rig.open();
    const auto path = rig.place("VST3/Crash.vst3");
    list->record(setAside(path, stamp(1), PluginFailure::Kind::crashed, "loading the code", 11));
    CHECK_FALSE(list->needsScan(path, stamp(1)));

    list->tryAgain(path);
    CHECK(list->file(path)->again);
    CHECK(list->needsScan(path, stamp(1)));
    list->record(found(path, stamp(1), {effect(gainId, "Gain")}));
    CHECK_FALSE(list->file(path)->again);
    CHECK_FALSE(list->needsScan(path, stamp(1)));

    const auto shell = rig.place("VST3/Shell.vst3");
    auto partial = found(shell, stamp(5), {effect(gainId, "Gain")});
    partial.complete = false;
    list->record(partial);
    CHECK(list->needsScan(shell, stamp(5)));
}

TEST_CASE("A file changed after signing is looked at again once the computer has restarted")
{
    Rig rig;
    auto list = rig.open();
    const auto path = rig.place("Components/Signed.component");
    auto file = found(path, stamp(1), {});
    file.outcome = PluginFile::Outcome::changedAfterSigning;
    list->record(file);
    auto kept = found(rig.place("VST3/Old.vst3"), stamp(1), {});
    kept.outcome = PluginFile::Outcome::otherArchitecture;
    list->record(kept);

    rig.time += 2h;
    CHECK_FALSE(list->needsScan(path, stamp(1)));
    rig.started += 3h;
    CHECK(list->needsScan(path, stamp(1)));
    CHECK_FALSE(list->needsScan(rig.place("VST3/Old.vst3"), stamp(1)));
}

TEST_CASE("Scanning a plugin again unchanged keeps when it was first found and when it last changed")
{
    Rig rig;
    auto list = rig.open();
    const auto path = rig.place("VST3/Gain.vst3");
    const std::int64_t modified = 1'761'000'000'123'456'789;
    list->record(found(path, stamp(modified), {effect(gainId, "Gain")}));
    const auto first = *list->find({"VST3", gainId});
    CHECK(first.firstSeen == rig.time);
    CHECK(first.updated ==
          Clock::time_point(std::chrono::duration_cast<Clock::duration>(std::chrono::nanoseconds(modified))));

    // Everything scanned again a week later.
    rig.time += 24h * 7;
    list->clear();
    CHECK(list->files().empty());
    list->record(found(path, stamp(modified), {effect(gainId, "Gain")}));
    const auto again = *list->find({"VST3", gainId});
    CHECK(again.firstSeen == first.firstSeen);
    CHECK(again.updated == first.updated);

    // Its binary replaced: updated follows the file, first seen stays.
    list->record(found(path, stamp(modified + 3'600'000'000'000), {effect(gainId, "Gain")}));
    const auto updated = *list->find({"VST3", gainId});
    CHECK(updated.firstSeen == first.firstSeen);
    CHECK(updated.updated == first.updated + 1h);
}

TEST_CASE("A folder read through drops what's gone from it and keeps what's on a drive not connected")
{
    Rig rig;
    auto list = rig.open();
    const auto kept = rig.place("VST3/Kept.vst3");
    const auto gone = rig.place("VST3/Sub/Gone.vst3");
    const auto away = rig.place("Drive/VST3/Away.vst3");
    const auto alsoAway = rig.place("Drive/VST3/AlsoAway.vst3");
    list->record(found(kept, stamp(1), {effect(gainId, "Kept")}));
    list->record(found(gone, stamp(1), {effect(delayId, "Gone")}));
    list->record(found(away, stamp(1), {effect("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", "Away")}));
    list->record(found(alsoAway, stamp(1), {effect("BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB", "Also Away")}));

    list->folderRead(rig.place("VST3"), {kept});
    list->folderUnreadable(rig.place("Drive/VST3/"));

    CHECK(list->file(kept));
    CHECK_FALSE(list->file(gone));
    CHECK_FALSE(list->find({"VST3", delayId}));
    REQUIRE(list->file(away));
    CHECK_FALSE(list->file(away)->connected);
    CHECK_FALSE(list->file(alsoAway)->connected);
    REQUIRE(list->find({"VST3", "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"}));
    CHECK_FALSE(list->find({"VST3", "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"})->connected);
    CHECK(list->file(kept)->connected);

    // The drive back with one of the two on it.
    list->folderRead(rig.place("Drive/VST3"), {away});
    CHECK(list->file(away)->connected);
    CHECK_FALSE(list->file(alsoAway));

    // A folder with a name that only begins like another's isn't inside it.
    const auto beside = rig.place("VST3 Extra/Beside.vst3");
    list->record(found(beside, stamp(1), {effect(delayId, "Beside")}));
    list->folderRead(rig.place("VST3"), {kept});
    CHECK(list->file(beside));
}

TEST_CASE("A plugin not found by identity can be looked for by name, and is never found by it on its own")
{
    Rig rig;
    auto list = rig.open();
    list->record(found(rig.place("VST3/Gain.vst3"), stamp(1), {effect(gainId, "Gain")}));

    // A card saved with an identity the plugin no longer has, under the same maker and name.
    const PluginIdentity old{"VST3", "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"};
    CHECK_FALSE(list->find(old));
    const auto guesses = list->findByName("VST3", "Test Maker", "Gain");
    REQUIRE(guesses.size() == 1);
    CHECK(guesses[0].plugin.identity.id == gainId);

    CHECK(list->findByName("AudioUnit", "Test Maker", "Gain").empty());
    CHECK(list->findByName("VST3", "Other Maker", "Gain").empty());
    CHECK(list->findByName("VST3", "Test Maker", "gain").empty());
}

TEST_CASE("The list is kept in its file, one entry to a line, and read back as it was")
{
    Rig rig;
    std::vector<PluginFile> files;
    std::vector<PluginEntry> plugins;
    {
        auto list = rig.open();
        auto bundle = found(rig.place("VST3/Bundle.vst3"), stamp(1'761'000'000'000'000'001, "4.5.6", 123'456'789),
                            {effect(gainId, "Gain"), effect(delayId, "Delay")});
        bundle.plugins[1].kind = PluginKind::instrument;
        bundle.plugins[1].layouts = {};
        bundle.plugins[1].editor = false;
        bundle.stamp.manifestModified = 1'761'000'000'000'000'002;
        bundle.architectures = {"arm64", "x86_64"};
        bundle.setAside.push_back({effect("CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC", "Third"),
                                   {PluginFailure::Kind::noAnswer, "making plugin 3 of 3"}});
        list->record(bundle);
        list->record(setAside(rig.place("VST3/Crash.vst3"), stamp(2), PluginFailure::Kind::crashed, "loading the code",
                              0xC0000005));
        auto mine = found(rig.place("Mine/Nothing.vst3"), stamp(3), {});
        mine.outcome = PluginFile::Outcome::nothing;
        mine.userFolder = true;
        list->record(mine);
        list->tryAgain(rig.place("Mine/Nothing.vst3"));
        auto intel = found(rig.place("VST3/Intel.vst3"), stamp(4), {});
        intel.outcome = PluginFile::Outcome::otherArchitecture;
        intel.architectures = {"x86_64"};
        list->record(intel);
        auto partial =
            found(rig.place("VST3/Partial.vst3"), stamp(5), {effect("DDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDD", "Part")});
        partial.complete = false;
        list->record(partial);
        list->folderUnreadable(rig.place("Mine"));
        files = list->files();
        plugins = list->plugins();
        REQUIRE(list->hasChanges());
        list->save();
        CHECK_FALSE(list->hasChanges());
    }

    const auto text = files::read(rig.file);
    const auto written = lines(text);
    REQUIRE(written.size() >= 4);
    CHECK(written[0] == "{");
    CHECK(written[1] == "\"format\": 1,");
    int entries = 0;
    for (const auto& line : written)
        if (line.starts_with("{\""))
            ++entries;
    CHECK(entries == 5 + 3);   // five files, and when each of the three plugins found was first found

    auto again = rig.open();
    CHECK(again->files() == files);
    CHECK(again->plugins() == plugins);
    CHECK_FALSE(again->hasChanges());
    CHECK(rig.logged.empty());
}

TEST_CASE("The file before is kept as a backup, and read when the list's own file is gone")
{
    Rig rig;
    {
        auto list = rig.open();
        list->record(found(rig.place("VST3/Gain.vst3"), stamp(1), {effect(gainId, "Gain")}));
        list->save();
        CHECK_FALSE(std::filesystem::exists(rig.backup));
        list->record(found(rig.place("VST3/Delay.vst3"), stamp(1), {effect(delayId, "Delay")}));
        list->save();
    }
    REQUIRE(std::filesystem::exists(rig.backup));
    CHECK(rig.open()->files().size() == 2);

    std::filesystem::remove(rig.file);
    auto list = rig.open();
    REQUIRE(list->files().size() == 1);
    CHECK(list->files()[0].path == rig.place("VST3/Gain.vst3"));
    CHECK_FALSE(rig.logged.empty());
}

TEST_CASE("A list cut short at any byte loads, losing only the entry it was cut in")
{
    Rig rig;
    std::vector<PluginFile> files;
    {
        auto list = rig.open();
        list->record(found(rig.place("VST3/Alpha.vst3"), stamp(1), {effect(gainId, "Alpha")}));
        list->record(found(rig.place("VST3/Mango.vst3"), stamp(2), {effect(delayId, "Mango")}));
        list->record(setAside(rig.place("VST3/Zebra.vst3"), stamp(3), PluginFailure::Kind::crashed, "loading", 11));
        files = list->files();
        list->save();
    }
    const auto whole = files::read(rig.file);
    REQUIRE(files.size() == 3);
    const auto withoutComma = [](std::string line) {
        if (line.ends_with(","))
            line.pop_back();
        return line;
    };
    std::set<std::string> entryLines;
    for (const auto& line : lines(whole))
        if (line.starts_with("{\"path\""))
            entryLines.insert(withoutComma(line));
    REQUIRE(entryLines.size() == 3);

    for (std::size_t length = 0; length < whole.size(); ++length)
    {
        const auto cut = whole.substr(0, length);
        writePlainly(rig.file, cut);

        // Each file entry whose line is there whole is read as it was.
        std::size_t wholeLines = 0;
        for (const auto& line : lines(cut))
            if (entryLines.contains(withoutComma(line)))
                ++wholeLines;

        std::unique_ptr<PluginList> list;
        REQUIRE_NOTHROW(list = rig.open());
        const auto loaded = list->files();
        CHECK(loaded.size() == wholeLines);
        for (const auto& file : loaded)
            CHECK(std::find(files.begin(), files.end(), file) != files.end());
    }
}

TEST_CASE("A list cut short takes the entries it lost from the backup")
{
    Rig rig;
    std::vector<PluginFile> files;
    {
        auto list = rig.open();
        list->record(found(rig.place("VST3/Alpha.vst3"), stamp(1), {effect(gainId, "Alpha")}));
        list->record(found(rig.place("VST3/Mango.vst3"), stamp(2), {effect(delayId, "Mango")}));
        list->save();
        list->record(found(rig.place("VST3/Zebra.vst3"), stamp(3), {effect(std::string(32, 'E'), "Zebra")}));
        files = list->files();
        list->save();
    }
    const auto whole = files::read(rig.file);
    const auto alphaEnds = whole.find("Alpha.vst3");
    REQUIRE(alphaEnds != std::string::npos);

    // Cut inside the second entry: the first is read from the list, the second from the backup, and the third, which
    // the backup never held, is lost.
    files::writeSafely(rig.file, whole.substr(0, whole.find("Mango.vst3")));
    auto list = rig.open();
    const auto loaded = list->files();
    REQUIRE(loaded.size() == 2);
    CHECK(loaded[0] == files[0]);
    CHECK(loaded[1] == files[1]);
    CHECK(list->hasChanges());
    CHECK_FALSE(rig.logged.empty());

    // Saving it writes the list whole again and keeps the backup as it was, the last good one.
    const auto backup = files::read(rig.backup);
    list->save();
    CHECK(files::read(rig.backup) == backup);
    CHECK(rig.open()->files() == loaded);
}

TEST_CASE("Bytes flipped anywhere in the list never stop it loading, and every entry they missed reads back as it was")
{
    Rig rig;
    std::vector<PluginFile> files;
    {
        auto list = rig.open();
        list->record(found(rig.place("VST3/Alpha.vst3"), stamp(1), {effect(gainId, "Alpha")}));
        list->record(found(rig.place("VST3/Mango.vst3"), stamp(2), {effect(delayId, "Mango")}));
        list->record(setAside(rig.place("VST3/Zebra.vst3"), stamp(3), PluginFailure::Kind::crashed, "loading", 11));
        files = list->files();
        list->save();
    }
    const auto whole = files::read(rig.file);
    const auto wholeLines = lines(whole);

    std::mt19937 random(20261029);
    for (int trial = 0; trial < 3000; ++trial)
    {
        auto damaged = whole;
        const auto at = std::uniform_int_distribution<std::size_t>(0, damaged.size() - 1)(random);
        damaged[at] = static_cast<char>(damaged[at] ^ (1 << std::uniform_int_distribution<int>(0, 7)(random)));
        writePlainly(rig.file, damaged);

        std::unique_ptr<PluginList> list;
        REQUIRE_NOTHROW(list = rig.open());
        const auto loaded = list->files();

        // The line the flip fell in, and the file entries on the other lines, each read as it was.
        const auto line = std::size_t(std::count(whole.begin(), whole.begin() + std::ptrdiff_t(at), '\n'));
        for (const auto& file : files)
        {
            const auto name = paths::toUtf8(file.path.filename());
            const bool hit = wholeLines[line].find(name) != std::string::npos ||
                             (whole[at] == '\n' && line + 1 < wholeLines.size() &&
                              wholeLines[line + 1].find(name) != std::string::npos);
            if (hit)
                continue;
            const auto match = std::find_if(loaded.begin(), loaded.end(),
                                            [&](const PluginFile& one) { return one.path == file.path; });
            REQUIRE(match != loaded.end());
            CHECK(*match == file);
        }
    }
}

TEST_CASE("An entry that can't be read is left out and said, and the rest are read")
{
    Rig rig;
    {
        auto list = rig.open();
        list->record(found(rig.place("VST3/Alpha.vst3"), stamp(1), {effect(gainId, "Alpha")}));
        list->record(found(rig.place("VST3/Mango.vst3"), stamp(2), {effect(delayId, "Mango")}));
        list->save();
    }
    auto text = files::read(rig.file);
    const auto size = text.find("\"size\":4096", text.find("Mango"));
    REQUIRE(size != std::string::npos);
    text.replace(size, 11, "\"size\":\"big\"");
    files::writeSafely(rig.file, text);

    auto list = rig.open();
    REQUIRE(list->files().size() == 1);
    CHECK(list->files()[0].path == rig.place("VST3/Alpha.vst3"));
    REQUIRE(rig.logged.size() == 1);
    CHECK(rig.logged[0].find("Mango") != std::string::npos);
}

TEST_CASE("An entry that can't be read is taken from the backup, and its file never becomes the backup")
{
    Rig rig;
    std::vector<PluginFile> files;
    {
        auto list = rig.open();
        list->record(found(rig.place("VST3/Alpha.vst3"), stamp(1), {effect(gainId, "Alpha")}));
        list->record(found(rig.place("VST3/Mango.vst3"), stamp(2), {effect(delayId, "Mango")}));
        list->save();
        list->record(found(rig.place("VST3/Alpha.vst3"), stamp(5), {effect(gainId, "Alpha")}));
        files = list->files();
        list->save();
    }
    auto text = files::read(rig.file);
    const auto size = text.find("\"size\":4096", text.find("Mango"));
    REQUIRE(size != std::string::npos);
    text.replace(size, 11, "\"size\":\"big\"");
    files::writeSafely(rig.file, text);
    const auto backup = files::read(rig.backup);

    auto list = rig.open();
    CHECK(list->files() == files);
    CHECK(rig.logged.size() == 2);
    list->record(found(rig.place("VST3/Zebra.vst3"), stamp(1), {effect(std::string(32, 'E'), "Zebra")}));
    list->save();
    CHECK(files::read(rig.backup) == backup);
}

TEST_CASE("Numbers past what a time can hold, as a damaged file can say, are read without harm")
{
    Rig rig;
    {
        auto list = rig.open();
        auto file = setAside(rig.place("VST3/Crash.vst3"), stamp(1), PluginFailure::Kind::crashed, "loading", 11);
        file.outcome = PluginFile::Outcome::changedAfterSigning;
        list->record(file);
        list->record(found(rig.place("VST3/Gain.vst3"), stamp(1), {effect(gainId, "Gain")}));
        list->save();
    }
    auto text = files::read(rig.file);
    const auto most = std::to_string(std::numeric_limits<std::int64_t>::max());
    const auto least = std::to_string(std::numeric_limits<std::int64_t>::min());
    for (const auto& [name, value] : std::vector<std::pair<std::string, std::string>>{
             {"\"date\":", most}, {"\"computerStarted\":", least}, {"\"firstSeen\":", least}})
    {
        const auto at = text.find(name);
        REQUIRE(at != std::string::npos);
        const auto end = text.find_first_of(",}", at);
        text.replace(at + name.size(), end - at - name.size(), value);
    }
    files::writeSafely(rig.file, text);

    auto list = rig.open();
    REQUIRE(list->files().size() == 2);
    CHECK(list->needsScan(rig.place("VST3/Crash.vst3"), stamp(1)));
    CHECK(list->file(rig.place("VST3/Crash.vst3"))->failure->date > rig.time);
    REQUIRE(list->find({"VST3", gainId}));
    CHECK(list->find({"VST3", gainId})->firstSeen < rig.time);
}

TEST_CASE("Fields the list doesn't know are kept in the entries it doesn't change")
{
    Rig rig;
    {
        auto list = rig.open();
        list->record(found(rig.place("VST3/Alpha.vst3"), stamp(1), {effect(gainId, "Alpha")}));
        list->record(found(rig.place("VST3/Mango.vst3"), stamp(2), {effect(delayId, "Mango")}));
        list->save();
    }
    auto text = files::read(rig.file);
    for (const auto* name : {"Alpha.vst3", "Mango.vst3"})
    {
        const auto line = text.rfind('\n', text.find(name)) + 1;
        text.insert(line + 1, "\"later\":{\"kept\":[1,2]},");
    }
    text.insert(text.find("\"files\""), "\"laterList\": [7],\n");
    files::writeSafely(rig.file, text);

    {
        auto list = rig.open();
        CHECK(list->files().size() == 2);
        list->record(found(rig.place("VST3/Mango.vst3"), stamp(3), {effect(delayId, "Mango")}));
        list->save();
    }
    const auto after = files::read(rig.file);
    const auto alpha = after.substr(after.rfind('\n', after.find("Alpha.vst3")));
    CHECK(alpha.substr(0, alpha.find('\n', 1)).find("\"later\":{\"kept\":[1,2]}") != std::string::npos);
    const auto mango = after.substr(after.rfind('\n', after.find("Mango.vst3")));
    CHECK(mango.substr(0, mango.find('\n', 1)).find("later") == std::string::npos);
    CHECK(after.find("\"laterList\": [7]") != std::string::npos);
}

TEST_CASE("The list written with a German locale reads back the same")
{
    const std::string before = std::setlocale(LC_ALL, nullptr);
    bool german = false;
    for (const char* name : {"de_DE.UTF-8", "de_DE.utf8", "de_DE", "German_Germany.1252"})
        if (std::setlocale(LC_ALL, name) != nullptr)
        {
            german = true;
            break;
        }

    Rig rig;
    std::vector<PluginFile> files;
    {
        auto list = rig.open();
        list->record(found(rig.place("VST3/Alpha.vst3"), stamp(1'761'000'000'123'456'789, "1.234,5", 1'234'567),
                           {effect(gainId, "Alpha 1.000,5")}));
        files = list->files();
        list->save();
    }
    std::setlocale(LC_ALL, "C");
    const auto text = files::read(rig.file);
    CHECK(text.find("\"size\":1234567") != std::string::npos);
    CHECK(rig.open()->files() == files);
    std::setlocale(LC_ALL, before.c_str());
    if (!german)
        WARN("No German locale on this system: written in the default one.");
}

TEST_CASE("Nothing is written while nothing has changed")
{
    Rig rig;
    auto list = rig.open();
    CHECK_FALSE(list->hasChanges());
    list->save();
    CHECK_FALSE(std::filesystem::exists(rig.file));

    const auto file = found(rig.place("VST3/Gain.vst3"), stamp(1), {effect(gainId, "Gain")});
    list->record(file);
    list->save();
    list->record(file);
    list->folderRead(rig.place("VST3"), {rig.place("VST3/Gain.vst3")});
    list->tryAgain(rig.place("VST3/None.vst3"));
    CHECK_FALSE(list->hasChanges());
}

TEST_CASE("Versions are compared piece by piece, numbers as numbers")
{
    CHECK(compareVersions("1.10", "1.9") > 0);
    CHECK(compareVersions("1.9", "1.10") < 0);
    CHECK(compareVersions("2.0", "2.0.0") == 0);
    CHECK(compareVersions("2.0.1", "2.0") > 0);
    CHECK(compareVersions("1.01", "1.1") == 0);
    CHECK(compareVersions("1.0b10", "1.0b2") > 0);
    CHECK(compareVersions("1.0.0", "1.0.0-beta") > 0);
    CHECK(compareVersions("1.0.1", "1.0beta") > 0);
    CHECK(compareVersions("", "1") < 0);
    CHECK(compareVersions("", "") == 0);
    CHECK(compareVersions("v2", "v10") < 0);
    CHECK(compareVersions("123456789012345678901234567890", "99999999999999999999") > 0);
}
