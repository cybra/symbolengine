#include "Scratch.h"
#include "TestMainThread.h"

#include <symbolengine/core/ChildProcess.h>
#include <symbolengine/core/Files.h>
#include <symbolengine/core/Json.h>
#include <symbolengine/core/Paths.h>
#include <symbolengine/core/System.h>
#include <symbolengine/core/Text.h>
#include <symbolengine/host/PluginScanner.h>
#include <symbolengine/host/Vst3Format.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace symbolengine;
using namespace std::chrono_literals;

namespace
{

const std::filesystem::path bundles = SYMBOLENGINE_MISBEHAVING;

// Where this system's VST3 loader looks for a bundle's binary, relative to the bundle.
std::filesystem::path binaryIn(const std::string& name)
{
#if defined(__APPLE__)
    static_cast<void>(name);
    return std::filesystem::path("Contents") / "MacOS" / "SymbolEngine Misbehaving";
#elif defined(_WIN32)
    return std::filesystem::path("Contents") / (system::architecture() == "arm64" ? "arm64-win" : "x86_64-win") /
           (name + ".vst3");
#else
    return std::filesystem::path("Contents") / (system::architecture() == "arm64" ? "aarch64-linux" : "x86_64-linux") /
           (name + ".so");
#endif
}

// A copy of the good test bundle in the folder under the name, which says what it does.
std::filesystem::path install(const std::filesystem::path& folder, const std::string& name)
{
    const auto from = bundles / "Good A.vst3";
    const auto to = folder / (name + ".vst3");
    std::filesystem::create_directories(folder);
    std::filesystem::copy(from, to, std::filesystem::copy_options::recursive);
#if !defined(__APPLE__)
    std::filesystem::rename(to / binaryIn("Good A"), to / binaryIn(name));
#endif
    return to;
}

// A bundle whose binary is the bytes given, where this system's loader looks for it.
std::filesystem::path synthetic(const std::filesystem::path& folder, const std::string& name, const std::string& bytes)
{
    const auto bundle = folder / (name + ".vst3");
    const auto binary = bundle / binaryIn(name);
    std::filesystem::create_directories(binary.parent_path());
    files::writeSafely(binary, bytes);
    return bundle;
}

std::string little(std::uint64_t value, int size)
{
    std::string bytes;
    for (int i = 0; i < size; ++i)
        bytes += static_cast<char>((value >> (8 * i)) & 0xFF);
    return bytes;
}

std::string big(std::uint64_t value, int size)
{
    std::string bytes;
    for (int i = size - 1; i >= 0; --i)
        bytes += static_cast<char>((value >> (8 * i)) & 0xFF);
    return bytes;
}

// The first bytes of a binary built for an architecture, as each system's format lays them out.
std::string machO(std::uint32_t cpu)
{
    return little(0xFEEDFACF, 4) + little(cpu, 4) + std::string(24, '\0');
}

std::string fatMachO(std::vector<std::uint32_t> cpus)
{
    auto bytes = big(0xCAFEBABE, 4) + big(cpus.size(), 4);
    for (const auto cpu : cpus)
        bytes += big(cpu, 4) + big(0, 4) + big(4096, 4) + big(64, 4) + big(12, 4);
    return bytes + std::string(64, '\0');
}

std::string pe(std::uint16_t machine)
{
    std::string bytes = "MZ" + std::string(0x3A, '\0') + little(0x80, 4);
    bytes.resize(0x80, '\0');
    return bytes + "PE" + std::string(2, '\0') + little(machine, 2) + std::string(64, '\0');
}

std::string elf(std::uint16_t machine)
{
    return std::string("\x7F"
                       "ELF\x02\x01\x01",
                       7) +
           std::string(11, '\0') + little(machine, 2) + std::string(44, '\0');
}

constexpr std::uint32_t armCpu = 0x0100000C, intelCpu = 0x01000007;

// What this system's binaries are built for, and the bytes of one built only for another architecture.
std::string elsewhere()
{
    const bool arm = system::architecture() == "arm64";
#if defined(__APPLE__)
    return machO(arm ? intelCpu : armCpu);
#elif defined(_WIN32)
    return pe(arm ? 0x8664 : 0xAA64);
#else
    return elf(arm ? 62 : 183);
#endif
}

// A walk and a scan over the test's own folder, into a list of the test's own, its main thread the test's.
struct Rig
{
    ScratchFolder scratch;
    std::filesystem::path plugins = scratch.path / "Plugins";
    std::filesystem::path settings = scratch.path / "Settings";
    TestMainThread main;
    Vst3Format vst3{{"SymbolEngine tests", main.post(), main.isCurrent()}};
    PluginList list{{settings / "Plugins.json", "1.0", nullptr, nullptr, nullptr}};
    std::unique_ptr<PluginScanner> scanner;
    std::optional<PluginScanEnd> end;
    std::vector<std::string> steps;

    explicit Rig(std::chrono::milliseconds saveEvery = 10s)
    {
        scanner = std::make_unique<PluginScanner>(
            list, PluginScanner::Options{{SYMBOLENGINE_SCAN_WORKER, {}, scratch.path / "workers.txt", 10s, main.post()},
                                         PluginSetup{48000.0, 256, false},
                                         50,
                                         std::int64_t(2) * 1024 * 1024 * 1024,
                                         saveEvery});
        scanner->onFinished = [this](const PluginScanEnd& how) { end = how; };
        scanner->onProgress = [this](const std::filesystem::path&, const std::string& step) { steps.push_back(step); };
    }

    PluginWalk walk(bool user = false) { return PluginScanner::walk({{&vst3, {plugins, user}}}, list); }

    bool scan(std::chrono::milliseconds limit = 60s)
    {
        end.reset();
        REQUIRE(scanner->scan(walk().files));
        return main.turnUntil([this] { return end.has_value(); }, limit);
    }

    std::vector<std::string> names(const PluginWalk& walked) const
    {
        std::vector<std::string> all;
        for (const auto& file : walked.files)
            all.push_back(paths::toUtf8(file.path.filename()));
        return all;
    }
};

std::filesystem::path real(const std::filesystem::path& path)
{
    return std::filesystem::weakly_canonical(path);
}

bool linked(const std::filesystem::path& to, const std::filesystem::path& link)
{
    std::error_code error;
    std::filesystem::create_directory_symlink(to, link, error);
    if (error)
        WARN("No symbolic link could be made here: " << error.message());
    return !error;
}

}

TEST_CASE("A walk finds each plugin file once by its real path: through links and a loop, never inside a bundle, never "
          "in a hidden folder")
{
    Rig rig;
    const auto first = install(rig.plugins, "First");
    const auto second = install(rig.plugins / "Sub", "Second");
    install(first / "Contents" / "Resources", "Inside");
    install(rig.plugins / ".hidden", "Hidden");
    const bool links = linked(rig.plugins, rig.plugins / "Sub" / "Loop") &&
                       linked(second, rig.plugins / "One link.vst3") &&
                       linked(second, rig.plugins / "Sub" / "Other link.vst3");

    const auto walked = rig.walk();
    CHECK(walked.refused.empty());
    std::vector<std::filesystem::path> found;
    for (const auto& file : walked.files)
        found.push_back(file.path);
    CHECK(found == std::vector<std::filesystem::path>{real(first), real(second)});
    CHECK(walked.files[0].format == "VST3");
    const auto& architectures = walked.files[0].architectures;
    CHECK(std::find(architectures.begin(), architectures.end(), system::architecture()) != architectures.end());
    CHECK(walked.files[0].stamp.size > 0);
    static_cast<void>(links);
}

TEST_CASE("A walk never enters a volume or a home folder")
{
    Rig rig;
    const auto home = paths::systemFolder(paths::Folder::home);
    const auto walked =
        PluginScanner::walk({{&rig.vst3, {home.root_path(), true}}, {&rig.vst3, {home, true}}}, rig.list);
    CHECK(walked.files.empty());
    CHECK(walked.refused.size() == 2);
}

TEST_CASE("A file's stamp follows the binary inside its bundle, by its size, its time either way and its declared "
          "version, and not the bundle's own time")
{
    Rig rig;
    const auto bundle = install(rig.plugins, "Stamped");
    const auto binary = bundle / binaryIn("Stamped");
    PluginError error;
    const auto read = [&] {
        const auto facts = rig.vst3.examine(bundle, error);
        REQUIRE(facts);
        return facts->stamp;
    };
    const auto before = read();
    CHECK(before.size == static_cast<std::int64_t>(std::filesystem::file_size(binary)));
    CHECK(before.modified != 0);

    // Touched: the bundle's own time moves, its binary's doesn't.
    std::filesystem::last_write_time(bundle, std::filesystem::last_write_time(bundle) + 1h);
    CHECK(read() == before);

    // An older build put back: the same size, an earlier time.
    const auto time = std::filesystem::last_write_time(binary);
    std::filesystem::last_write_time(binary, time - 1h);
    CHECK(read() != before);
    CHECK(read().modified < before.modified);
    std::filesystem::last_write_time(binary, time);
    CHECK(read() == before);

    // The binary replaced with the bundle's time kept.
    const auto bundleTime = std::filesystem::last_write_time(bundle);
    auto contents = files::read(binary);
    files::writeSafely(binary, contents + std::string(16, '\0'));
    std::filesystem::last_write_time(bundle, bundleTime);
    const auto replaced = read();
    CHECK(replaced.size == before.size + 16);

    // Only its declared version, in the manifest a VST3 may carry.
    files::writeSafely(bundle / "Contents" / "Resources" / "moduleinfo.json",
                       R"({"Name": "Stamped", "Version": "2.5.1"})");
    const auto versioned = read();
    CHECK(versioned.version == "2.5.1");
    CHECK(versioned.manifestModified != 0);
    CHECK(versioned.size == replaced.size);
}

TEST_CASE("A binary's architectures are read from its headers, whichever system built it")
{
    Rig rig;
    const std::vector<std::pair<std::string, std::vector<std::string>>> expected{
        {machO(intelCpu), {"x86_64"}},
        {machO(armCpu), {"arm64"}},
        {fatMachO({intelCpu, armCpu}), {"x86_64", "arm64"}},
        {fatMachO({armCpu, intelCpu, 7, 12}), {"arm64", "x86_64", "x86", "arm"}},
        {pe(0x8664), {"x86_64"}},
        {pe(0xAA64), {"arm64"}},
        {pe(0x14C), {"x86"}},
        {elf(62), {"x86_64"}},
        {elf(183), {"arm64"}},
        {std::string(64, 'x'), {}},
        {big(0xCAFEBABE, 4) + big(51, 4) + std::string(64, '\0'), {}},
    };
    int number = 0;
    for (const auto& [bytes, architectures] : expected)
    {
        const auto bundle = synthetic(rig.plugins, "Binary " + std::to_string(++number), bytes);
        PluginError error;
        const auto facts = rig.vst3.examine(bundle, error);
        INFO(number);
        REQUIRE(facts);
        CHECK(facts->architectures == architectures);
    }

    PluginError error;
    std::filesystem::create_directories(rig.plugins / "Empty.vst3" / "Contents");
    CHECK_FALSE(rig.vst3.examine(rig.plugins / "Empty.vst3", error));
    CHECK(error);
}

TEST_CASE("Files built only for another architecture are listed as that, never given to a worker")
{
    Rig rig;
    const auto other = synthetic(rig.plugins, "Elsewhere", elsewhere());
    install(rig.plugins, "Here");
    const auto walked = rig.walk();
    CHECK(rig.names(walked) == std::vector<std::string>{"Here.vst3"});
    const auto listed = rig.list.file(real(other));
    REQUIRE(listed);
    CHECK(listed->outcome == PluginFile::Outcome::otherArchitecture);
    CHECK(listed->architectures.size() == 1);

    REQUIRE(rig.scan());
    CHECK(rig.scanner->workersStarted() == 1);
    CHECK(rig.list.file(real(other))->outcome == PluginFile::Outcome::otherArchitecture);
    CHECK(rig.list.plugins().size() == 1);
}

TEST_CASE("Files gone from a folder that can be read leave the list, and those in one that can't, or on a drive that "
          "isn't there, are kept")
{
    Rig rig;
    const auto kept = real(install(rig.plugins, "Kept"));
    const auto gone = real(install(rig.plugins, "Gone"));
    const auto locked = rig.scratch.path / "Locked";
    const auto away = real(install(locked, "Away"));
    const std::vector<PluginScanFolder> folders{{&rig.vst3, {rig.plugins, false}}, {&rig.vst3, {locked, true}}};
    REQUIRE(rig.scanner->scan(PluginScanner::walk(folders, rig.list).files));
    REQUIRE(rig.main.turnUntil([&] { return rig.end.has_value(); }, 60s));
    REQUIRE(rig.list.files().size() == 3);

    // One removed, and a folder that can't be read.
    std::filesystem::remove_all(gone);
#if !defined(_WIN32)
    std::filesystem::permissions(locked, std::filesystem::perms::none);
    PluginScanner::walk(folders, rig.list);
    std::filesystem::permissions(locked, std::filesystem::perms::owner_all);
    REQUIRE(rig.list.file(away));
    CHECK_FALSE(rig.list.file(away)->connected);
#else
    PluginScanner::walk(folders, rig.list);
#endif
    CHECK(rig.list.file(kept));
    CHECK_FALSE(rig.list.file(gone));

    // A file on a drive that isn't there is kept as not connected.
#if defined(_WIN32)
    std::filesystem::path drive;
    for (char letter = 'Z'; letter > 'D' && drive.empty(); --letter)
        if (std::error_code error; !std::filesystem::exists(std::string(1, letter) + ":\\", error))
            drive = std::string(1, letter) + ":\\VST3";
    REQUIRE_FALSE(drive.empty());
#elif defined(__APPLE__)
    const std::filesystem::path drive = "/Volumes/SymbolEngine test drive not there/VST3";
#else
    const std::filesystem::path drive = "/mnt/symbolengine-test-drive-not-there/VST3";
#endif
    rig.list.record({drive / "Elsewhere.vst3", "VST3", true, {1, 1, "1", 0}, {}, PluginFile::Outcome::found});
    PluginScanner::walk({{&rig.vst3, {drive, true}}}, rig.list);
    REQUIRE(rig.list.file(drive / "Elsewhere.vst3"));
    CHECK_FALSE(rig.list.file(drive / "Elsewhere.vst3")->connected);

    // A folder that simply isn't there holds nothing, a user's as well as a format's own.
    std::filesystem::remove_all(rig.plugins);
    std::filesystem::remove_all(locked);
    PluginScanner::walk(folders, rig.list);
    CHECK_FALSE(rig.list.file(kept));
    CHECK_FALSE(rig.list.file(away));
}

TEST_CASE("A scan's order is by folder and name, with files that held many plugins and then those set aside last")
{
    Rig rig;
    const auto other = rig.scratch.path / "Other";
    for (const auto* name : {"Delta", "Alpha", "Charlie", "Bravo"})
        install(rig.plugins, name);
    install(other, "Aardvark");

    auto shell = rig.walk().files[2];
    REQUIRE(shell.path.filename() == "Charlie.vst3");
    PluginFile many{shell.path, "VST3", false, {}, {}, PluginFile::Outcome::found};
    many.plugins = {{{"VST3", std::string(32, '1')}}, {{"VST3", std::string(32, '2')}}};
    rig.list.record(many);
    PluginFile crashed{real(rig.plugins / "Alpha.vst3"),
                       "VST3",
                       false,
                       {},
                       {},
                       PluginFile::Outcome::setAside,
                       PluginFailure{PluginFailure::Kind::crashed, "loading the code"}};
    rig.list.record(crashed);

    const auto walked = PluginScanner::walk({{&rig.vst3, {rig.plugins, false}}, {&rig.vst3, {other, true}}}, rig.list);
    CHECK(rig.names(walked) ==
          std::vector<std::string>{"Bravo.vst3", "Delta.vst3", "Aardvark.vst3", "Charlie.vst3", "Alpha.vst3"});
    CHECK(walked.files[2].userFolder);
}

TEST_CASE("A scan saves the list as it goes and says it is under way until it ends")
{
    Rig rig(0ms);
    install(rig.plugins, "Slow 1 1000 ms");
    install(rig.plugins, "Slow 2 1000 ms");
    REQUIRE(rig.scanner->scan(rig.walk().files));
    REQUIRE(rig.main.turnUntil([&] { return rig.list.files().size() == 1; }, 30s));
    {
        const PluginList saved{{rig.settings / "Plugins.json", "1.0", nullptr, nullptr, nullptr}};
        CHECK(saved.scanUnderway());
        CHECK(saved.files().size() == 1);
    }
    REQUIRE(rig.main.turnUntil([&] { return rig.end.has_value(); }, 30s));
    const PluginList saved{{rig.settings / "Plugins.json", "1.0", nullptr, nullptr, nullptr}};
    CHECK_FALSE(saved.scanUnderway());
    CHECK(saved.files().size() == 2);
}

TEST_CASE(
    "A scan stopped at any point sets nothing aside, keeps nothing of the file in hand, and the next one finishes "
    "it")
{
    Rig rig;
    for (int i = 1; i <= 4; ++i)
        install(rig.plugins, "Slow " + std::to_string(i) + " 150 ms");
    install(rig.plugins, "Many 6 with 0 crashes 100 ms");

    // The first stop comes halfway through the module of many plugins, the rest at random.
    std::mt19937 random(29102026);
    for (int trial = 0; trial < 8; ++trial)
    {
        rig.end.reset();
        REQUIRE(rig.scanner->scan(rig.walk().files));
        if (trial == 0)
            REQUIRE(rig.main.turnUntil(
                [&] { return !rig.steps.empty() && rig.steps.back().starts_with("making plugin 4 of 6"); }));
        else
            rig.main.turnUntil([] { return false; }, std::chrono::milliseconds(random() % 1200));
        rig.scanner->stop();
        REQUIRE(rig.main.turnUntil([&] { return rig.end.has_value(); }, 10s));
        for (const auto& file : rig.list.files())
        {
            INFO(paths::toUtf8(file.path));
            CHECK(file.setAside.empty());
            CHECK(file.outcome == PluginFile::Outcome::found);
            CHECK((file.complete || file.plugins.empty()));
        }
    }

    REQUIRE(rig.scan());
    CHECK_FALSE(rig.end->stopped);
    CHECK(rig.list.files().size() == 5);
    CHECK(rig.list.plugins().size() == 4 + 6);
    for (const auto& file : rig.list.files())
        CHECK((file.complete && file.setAside.empty()));
}

TEST_CASE("A program killed mid-scan leaves a list that reads, behind by no more than its save interval")
{
    ScratchFolder scratch;
    const auto folder = scratch.path / "Plugins";
    for (int i = 1; i <= 12; ++i)
        install(folder, "Slow " + std::to_string(i) + " 200 ms");
    const auto listFile = scratch.path / "Plugins.json";
    const auto said = scratch.path / "said.txt";
    std::error_code error;
    ChildProcess::Options options;
    options.arguments = {"scan", paths::toUtf8(folder), paths::toUtf8(listFile), "500"};
    options.outputFile = said;
    auto program = ChildProcess::start(SYMBOLENGINE_SCAN_WORKER, options, error);
    REQUIRE(program);

    // Killed once most files are recorded and before the last.
    const auto until = std::chrono::steady_clock::now() + 30s;
    for (;;)
    {
        const auto so = files::read(said, error);
        if (std::count(so.begin(), so.end(), '\n') >= 9 || std::chrono::steady_clock::now() > until)
            break;
        std::this_thread::sleep_for(20ms);
    }
    REQUIRE(program->kill());

    // Each line says how long after the scan began a file was recorded; those recorded more than the save interval
    // before the kill, with time for the line to be written, are in the file.
    const auto output = files::read(said);
    INFO(output);
    std::vector<long long> recorded;
    for (const auto line : text::split(output, "\n"))
        if (const auto at = text::parseNumber(text::trim(line)))
            recorded.push_back(static_cast<long long>(*at));
    REQUIRE(recorded.size() >= 9);
    const PluginList saved{{listFile, "1.0", nullptr, nullptr, nullptr}};
    CHECK(saved.scanUnderway());
    const auto expected =
        std::count_if(recorded.begin(), recorded.end(), [&](long long at) { return at < recorded.back() - 900; });
    CHECK(expected >= 2);
    CHECK(saved.files().size() >= static_cast<std::size_t>(expected));
    CHECK(saved.files().size() < 12);
}

TEST_CASE("A scan writes nothing in the program's folder but its list")
{
    Rig rig;
    for (int i = 1; i <= 20; ++i)
        install(rig.plugins, "Good " + std::to_string(i));
    std::filesystem::create_directories(rig.settings);
    files::writeSafely(rig.settings / "Settings.json", R"({"theme": "dark"})");
    files::writeSafely(rig.settings / "Broken.json", "{ not json");

    const auto snapshot = [&] {
        std::map<std::string, std::string> all;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(rig.settings))
            if (entry.is_regular_file())
                all[paths::toUtf8(entry.path().filename())] = files::read(entry.path());
        return all;
    };
    auto before = snapshot();
    REQUIRE(rig.scan(120s));
    CHECK(rig.list.plugins().size() == 20);
    auto after = snapshot();
    for (const auto* list : {"Plugins.json", "Plugins.json.backup"})
    {
        before.erase(list);
        after.erase(list);
    }
    CHECK(after == before);
}

namespace
{

// A manifest as Steinberg's tool writes one, trailing commas and all, with a comment of the kind its format allows, and
// the classes given.
std::string manifestOf(const std::string& classes)
{
    return "\xEF\xBB\xBF// Written by a tool.\n{\n  \"Name\": \"Module\",\n  \"Version\": \"7.8.9\",\n"
           "  /* What made it */\n  \"Factory Info\": {\n    \"Vendor\": \"Factory \\\"Maker // Inc\",\n"
           "    \"Flags\": {\n      \"Unicode\": true,\n    },\n  },\n  \"Classes\": [\n" +
           classes + "\n  ],\n}\n";
}

std::string classOf(const std::string& id, const std::string& name, const std::string& categories,
                    const std::string& category = "Audio Module Class", const std::string& maker = "")
{
    return "    {\n      \"CID\": \"" + id + "\",\n      \"Category\": \"" + category + "\",\n      \"Name\": \"" +
           name + "\",\n      \"Vendor\": \"" + maker +
           "\",\n      \"Version\": \"1.2.3\",\n      \"SDKVersion\": \"VST 3.7.12\",\n"
           "      \"Sub Categories\": [\n" +
           categories + "\n      ],\n    },\n";
}

const std::string oneId = "00112233445566778899AABBCCDDEEFF", twoId = "FFEEDDCCBBAA99887766554433221100";

void writeManifest(const std::filesystem::path& bundle, const std::string& text, bool older = false)
{
    const auto file =
        older ? bundle / "Contents" / "moduleinfo.json" : bundle / "Contents" / "Resources" / "moduleinfo.json";
    std::filesystem::create_directories(file.parent_path());
    files::writeSafely(file, text);
}

}

TEST_CASE("A module whose manifest declares only instruments is listed from it, its code never loaded")
{
    Rig rig;

    // Each would crash its worker if it were loaded.
    const auto bundle = install(rig.plugins, "Crash on load");
    writeManifest(bundle, manifestOf(classOf(oneId, "Keys // not a comment", "\"Instrument\",\"Synth\",") +
                                     classOf(twoId, "Keys Controller", "", "Component Controller Class") +
                                     classOf("0123456789abcdef0123456789abcdef", "Drums", "\"Instrument\",",
                                             "Audio Module Class", "Class Maker")));
    const auto older = install(rig.plugins, "Crash on load, older");
    writeManifest(older, manifestOf(classOf(oneId, "Keys", "\"Instrument\",")), true);
    const auto both = install(rig.plugins, "Crash on load, both");
    writeManifest(both, "", true);
    writeManifest(both, manifestOf(classOf(oneId, "Keys", "\"Instrument\",")));
    const auto emptyFirst = install(rig.plugins, "Crash on load, empty first");
    writeManifest(emptyFirst, "");
    writeManifest(emptyFirst, manifestOf(classOf(oneId, "Keys", "\"Instrument\",")), true);

    REQUIRE(rig.scan());
    REQUIRE(rig.end);
    CHECK_FALSE(rig.end->stopped);

    const auto listed = rig.list.file(real(bundle));
    REQUIRE(listed);
    CHECK(listed->outcome == PluginFile::Outcome::found);
    CHECK(listed->setAside.empty());
    REQUIRE(listed->plugins.size() == 2);
    CHECK(listed->plugins[0].identity.id == oneId);
    CHECK(listed->plugins[0].name == "Keys // not a comment");
    CHECK(listed->plugins[0].maker == "Factory \"Maker // Inc");
    CHECK(listed->plugins[0].version == "1.2.3");
    CHECK(listed->plugins[0].sdk == "VST 3.7.12");
    CHECK(listed->plugins[0].kind == PluginKind::instrument);
    CHECK(listed->plugins[1].identity.id == "0123456789ABCDEF0123456789ABCDEF");
    CHECK(listed->plugins[1].maker == "Class Maker");
    CHECK(listed->plugins[1].kind == PluginKind::instrument);

    CHECK(rig.list.file(real(older))->plugins.size() == 1);
    CHECK(rig.list.file(real(both))->plugins.size() == 1);
    CHECK(rig.list.file(real(emptyFirst))->plugins.size() == 1);
}

TEST_CASE("A module's code is loaded as ever unless its manifest clearly declares only instruments")
{
    Rig rig;
    const auto instrument = classOf(oneId, "Keys", "\"Instrument\",");
    const std::vector<std::pair<std::string, std::string>> manifests{
        {"An effect beside it", manifestOf(instrument + classOf(twoId, "Delay", "\"Fx\",\"Delay\","))},
        {"Effects by another word", manifestOf(classOf(oneId, "Delay", "\"Effects\","))},
        {"No categories", manifestOf(classOf(oneId, "Keys", ""))},
        {"A short ID", manifestOf(classOf("0011", "Keys", "\"Instrument\","))},
        {"An ID not in hex", manifestOf(classOf("00112233445566778899AABBCCDDEEGG", "Keys", "\"Instrument\","))},
        {"No name", manifestOf(classOf(oneId, "", "\"Instrument\","))},
        {"No audio classes", manifestOf(classOf(oneId, "Controller", "", "Component Controller Class"))},
        {"No classes", manifestOf("")},
        {"Classes of another kind", "{\"Classes\": 3}"},
        {"Not an object", "[1, 2]"},
        {"Keys unquoted", "{Classes: []}"},
        {"An empty file", ""},
    };
    for (const auto& [name, manifest] : manifests)
        writeManifest(install(rig.plugins, "Crash on load, " + name), manifest);
    REQUIRE(rig.scan());
    REQUIRE(rig.end);
    CHECK_FALSE(rig.end->stopped);

    for (const auto& [name, manifest] : manifests)
    {
        INFO(name);
        const auto listed = rig.list.file(real(rig.plugins / ("Crash on load, " + name + ".vst3")));
        REQUIRE(listed);
        CHECK(listed->outcome == PluginFile::Outcome::setAside);
        CHECK(listed->plugins.empty());
    }
}

TEST_CASE("A module's manifest says its version and its time wherever it lies, however it is written")
{
    Rig rig;
    const auto stampOf = [&](const std::filesystem::path& bundle) {
        PluginError error;
        const auto facts = rig.vst3.examine(bundle, error);
        REQUIRE(facts);
        return facts->stamp;
    };

    const auto newer = synthetic(rig.plugins, "Newer", elsewhere());
    CHECK(stampOf(newer).manifestModified == 0);
    writeManifest(newer, manifestOf(""));
    CHECK(stampOf(newer).manifestModified != 0);

    const auto older = synthetic(rig.plugins, "Older", elsewhere());
    writeManifest(older, manifestOf(""), true);
    CHECK(stampOf(older).manifestModified != 0);

#if !defined(_WIN32)
    // A version the module doesn't declare to the system otherwise.
    CHECK(stampOf(newer).version == "7.8.9");
    CHECK(stampOf(older).version == "7.8.9");
#endif
}

TEST_CASE("Damaged manifests cost a module nothing but the time of loading its code")
{
    Rig rig;
    const auto bundle = install(rig.plugins, "Damaged manifest");
    const auto base = manifestOf(classOf(oneId, "Keys", "\"Instrument\",\"Synth\","));

    // Listed by its factory when the manifest is no use, or from the manifest while it still clearly says this.
    const auto look = [&](const std::string& text) {
        writeManifest(bundle, text);
        PluginError error;
        const auto plugins = rig.vst3.pluginsIn(bundle, error);
        CHECK_FALSE(error);
        CHECK(plugins.size() == 1);
    };
    for (std::size_t size = 0; size <= base.size(); size += 3)
        look(base.substr(0, size));

    std::mt19937 random(20261030);
    for (int i = 0; i < 300; ++i)
    {
        auto bytes = base;
        for (int changes = 0; changes < 3; ++changes)
            bytes[random() % bytes.size()] = static_cast<char>(random());
        look(bytes);
    }

    // A comment or string never ended, and a comma on its own.
    look("{\"Classes\": [ /* never ended");
    look("{\"Classes\": [ \"never ended");
    look("{\"Classes\": [ // never ended");
    look("{\"Classes\": [,]}");
    look("\xEF\xBB");
    look(",");
}
