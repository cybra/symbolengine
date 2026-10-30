#include "Scratch.h"
#include "TestMainThread.h"

#include <symbolengine/core/Files.h>
#include <symbolengine/core/Paths.h>
#include <symbolengine/core/System.h>
#include <symbolengine/host/AudioUnitFormat.h>
#include <symbolengine/host/PluginScanWorker.h>
#include <symbolengine/host/PluginScanner.h>
#include <symbolengine/host/Vst3Format.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

using namespace symbolengine;
using namespace std::chrono_literals;

namespace
{

const std::filesystem::path bundles = SYMBOLENGINE_MISBEHAVING;
const std::filesystem::path library = SYMBOLENGINE_SIGNED_LIBRARY;

constexpr std::uint32_t loadDylib = 0xC, weakDylib = 0x80000018, reexportDylib = 0x8000001F, lazyDylib = 0x20,
                        upwardDylib = 0x80000023, runPath = 0x8000001C;

using Commands = std::vector<std::pair<std::uint32_t, std::string>>;

std::string quoted(const std::filesystem::path& path)
{
    return "'" + paths::toUtf8(path) + "'";
}

// Runs a tool to its end, saying nothing, and whether it succeeded.
bool tool(const std::string& command)
{
    return std::system((command + " >/dev/null 2>&1").c_str()) == 0;
}

void sign(const std::filesystem::path& path)
{
    REQUIRE(tool("codesign --force --sign - " + quoted(path)));
}

std::string little(std::uint64_t value, int size)
{
    std::string bytes;
    for (int i = 0; i < size; ++i)
        bytes += static_cast<char>((value >> (8 * i)) & 0xFF);
    return bytes;
}

std::uint64_t bigAt(const std::string& bytes, std::size_t at)
{
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 4; ++i)
        value = (value << 8) | static_cast<unsigned char>(bytes[at + i]);
    return value;
}

std::uint64_t littleAt(const std::string& bytes, std::size_t at)
{
    std::uint64_t value = 0;
    for (std::size_t i = 4; i > 0; --i)
        value = (value << 8) | static_cast<unsigned char>(bytes[at + i - 1]);
    return value;
}

std::uint64_t cpuOf(std::string_view architecture)
{
    return architecture == "arm64" ? 0x0100000C : 0x01000007;
}

std::string otherArchitecture()
{
    return system::architecture() == "arm64" ? "x86_64" : "arm64";
}

// Changes a byte of the architecture's code, in the first page's space after its load commands, which the signature's
// hash of that page covers.
void damage(const std::filesystem::path& file, const std::string& architecture = std::string(system::architecture()))
{
    std::error_code error;
    auto bytes = files::read(file, error);
    REQUIRE_FALSE(error);
    REQUIRE(bytes.size() > 8);

    std::size_t start = 0;
    if (bigAt(bytes, 0) == 0xCAFEBABE)
    {
        bool found = false;
        for (std::size_t i = 0; i < bigAt(bytes, 4); ++i)
            if (bigAt(bytes, 8 + i * 20) == cpuOf(architecture))
            {
                start = static_cast<std::size_t>(bigAt(bytes, 8 + i * 20 + 8));
                found = true;
            }
        REQUIRE(found);
    }
    const auto at = start + 32 + static_cast<std::size_t>(littleAt(bytes, start + 20)) + 8;
    REQUIRE(at < bytes.size());
    bytes[at] = static_cast<char>(bytes[at] ^ 0x55);

    std::fstream out(file, std::ios::binary | std::ios::in | std::ios::out);
    out.seekp(static_cast<std::streamoff>(at));
    out.put(bytes[at]);
    out.close();
    REQUIRE(out.good());
}

// A Mach-O binary of the program's architecture holding only the load commands, each with its one string.
std::string machO(const Commands& commands)
{
    std::string body;
    for (const auto& [command, text] : commands)
    {
        const std::size_t offset = command == runPath ? 12 : 24;
        auto padded = text + std::string(8 - (text.size() % 8), '\0');
        body += little(command, 4) + little(offset + padded.size(), 4) + little(offset, 4);
        if (offset == 24)
            body += std::string(12, '\0');
        body += padded;
    }
    return little(0xFEEDFACF, 4) + little(cpuOf(system::architecture()), 4) + little(0, 4) + little(8, 4) +
           little(commands.size(), 4) + little(body.size(), 4) + little(0, 4) + little(0, 4) + body;
}

// An Info.plist naming the bundle's executable, and an identifier ending as asked.
std::string infoNaming(const std::string& executable, const std::string& identifier)
{
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?><plist "
           "version=\"1.0\"><dict><key>CFBundleExecutable</key><string>" +
           executable + "</string><key>CFBundleIdentifier</key><string>dev.symbolengine.test." + identifier +
           "</string></dict></plist>";
}

// The bundle a VST3 is, with the binary given where a Mac's loader looks for it.
std::filesystem::path bundleOf(const std::filesystem::path& folder, const std::string& name, const std::string& binary)
{
    const auto bundle = folder / (name + ".vst3");
    const auto file = bundle / "Contents" / "MacOS" / name;
    std::filesystem::create_directories(file.parent_path());
    files::writeSafely(file, binary);
    return bundle;
}

std::filesystem::path binaryOf(const std::filesystem::path& bundle)
{
    return bundle / "Contents" / "MacOS" / bundle.stem();
}

// An Audio Unit's bundle holding a copy of the library built only for the architecture, signed.
std::filesystem::path audioUnitOf(const std::filesystem::path& folder, const std::string& name,
                                  const std::string& architecture)
{
    const auto bundle = folder / (name + ".component");
    const auto file = bundle / "Contents" / "MacOS" / name;
    std::filesystem::create_directories(file.parent_path());
    files::writeSafely(bundle / "Contents" / "Info.plist", infoNaming(name, "signed"));
    REQUIRE(tool("lipo -thin " + architecture + " " + quoted(library) + " -output " + quoted(file)));
    sign(file);
    return bundle;
}

std::filesystem::path real(const std::filesystem::path& path)
{
    return std::filesystem::canonical(path);
}

struct Rig
{
    ScratchFolder scratch;
    std::filesystem::path plugins = scratch.path / "Plugins";
    std::filesystem::path libraries = scratch.path / "Libraries";
    TestMainThread main;
    Vst3Format vst3{{"SymbolEngine tests", main.post(), main.isCurrent()}};
    AudioUnitFormat audioUnits{{main.post(), main.isCurrent(), nullptr}};

    Rig()
    {
        std::filesystem::create_directories(plugins);
        std::filesystem::create_directories(libraries);
    }

    // A signed copy of the library, built only for this program's architecture, under the name in the libraries'
    // folder, with its code changed if asked.
    std::filesystem::path libraryNamed(const std::string& name, bool changed)
    {
        const auto file = libraries / name;
        REQUIRE(tool("lipo -thin " + std::string(system::architecture()) + " " + quoted(library) + " -output " +
                     quoted(file)));
        sign(file);
        if (changed)
            damage(file);
        return file;
    }

    // What the check says of a plugin of the load commands given, in the plugins' folder.
    std::optional<std::filesystem::path> linking(const Commands& commands)
    {
        const auto bundle = bundleOf(plugins, "Linking " + std::to_string(++count), machO(commands));
        return vst3.changedAfterSigning(bundle);
    }

    int count = 0;
};

}

TEST_CASE("A plugin whose code no longer matches its signature is listed as that without its code loaded")
{
    Rig rig;
    const auto copy = [&](const std::string& from, const std::string& to) {
        const auto bundle = rig.plugins / (to + ".vst3");
        std::filesystem::copy(bundles / (from + ".vst3"), bundle, std::filesystem::copy_options::recursive);
        sign(bundle);
        return bundle;
    };

    // Were it loaded it would crash its worker, and be set aside as that.
    const auto changed = copy("Good A", "Crash on load");
    const auto binary = changed / "Contents" / "MacOS" / "SymbolEngine Misbehaving";
    damage(binary);

    // The bundle's other files are no part of what the system checks as it runs code.
    const auto good = copy("Good A", "Here");
    std::filesystem::create_directories(good / "Contents" / "Resources");
    files::writeSafely(good / "Contents" / "Resources" / "Added after signing.txt", "added");
    REQUIRE_FALSE(tool("codesign --verify --strict " + quoted(good)));

    PluginList list{{rig.scratch.path / "Settings" / "Plugins.json", "1.0", nullptr, nullptr, nullptr}};
    PluginScanner scanner(
        list,
        PluginScanner::Options{{SYMBOLENGINE_SCAN_WORKER, {}, rig.scratch.path / "workers.txt", 10s, rig.main.post()},
                               PluginSetup{48000.0, 256, false},
                               50,
                               std::int64_t(2) * 1024 * 1024 * 1024,
                               10s});
    std::map<std::filesystem::path, std::vector<std::string>> steps;
    std::optional<PluginScanEnd> end;
    scanner.onProgress = [&](const std::filesystem::path& file, const std::string& step) {
        steps[file].push_back(step);
    };
    scanner.onFinished = [&](const PluginScanEnd& how) { end = how; };

    const auto walked = PluginScanner::walk({{&rig.vst3, {rig.plugins, false}}}, list);
    REQUIRE(walked.files.size() == 2);
    REQUIRE(scanner.scan(walked.files));
    REQUIRE(rig.main.turnUntil([&] { return end.has_value(); }, 60s));

    const auto listed = list.file(real(changed));
    REQUIRE(listed);
    CHECK(listed->outcome == PluginFile::Outcome::changedAfterSigning);
    CHECK(listed->complete);
    CHECK(listed->plugins.empty());
    REQUIRE(listed->failure);
    CHECK(listed->failure->kind == PluginFailure::Kind::refused);
    CHECK(listed->failure->step == "checking its signature");
    CHECK(listed->failure->detail.find(paths::toUtf8(real(binary))) != std::string::npos);

    const auto saw = steps[real(changed)];
    CHECK(std::find(saw.begin(), saw.end(), "checking its signature") != saw.end());
    CHECK(std::find(saw.begin(), saw.end(), "loading the code") == saw.end());

    const auto kept = list.file(real(good));
    REQUIRE(kept);
    CHECK(kept->outcome == PluginFile::Outcome::found);
    CHECK(kept->plugins.size() == 1);
}

TEST_CASE("The libraries a plugin links outside the system are checked, each found as the system would find it")
{
    Rig rig;
    const auto changed = rig.libraryNamed("Changed.dylib", true);
    const auto whole = rig.libraryNamed("Whole.dylib", false);
    const auto name = paths::toUtf8(changed);
    const std::string up = "@loader_path/../../../../Libraries";

    CHECK(rig.linking({{loadDylib, paths::toUtf8(whole)}}) == std::nullopt);

    // However it is linked, the system loads it.
    for (const auto how : {loadDylib, weakDylib, reexportDylib, lazyDylib, upwardDylib})
        CHECK(rig.linking({{how, name}}) == real(changed));

    // By where the binary is, and through the folders it searches.
    CHECK(rig.linking({{loadDylib, up + "/Changed.dylib"}}) == real(changed));
    CHECK(rig.linking({{runPath, up}, {loadDylib, "@rpath/Changed.dylib"}}) == real(changed));
    CHECK(rig.linking(
              {{runPath, "/nowhere"}, {runPath, paths::toUtf8(rig.libraries)}, {loadDylib, "@rpath/Changed.dylib"}}) ==
          real(changed));
    CHECK(rig.linking({{loadDylib, "@rpath/Changed.dylib"}}) == std::nullopt);

    // Through a library that links it, searching the folders of the binary that loaded that library too.
    const auto middle = rig.libraries / "Middle.dylib";
    files::writeSafely(middle, machO({{loadDylib, "@rpath/Changed.dylib"}}));
    CHECK(rig.linking({{runPath, up}, {loadDylib, paths::toUtf8(middle)}}) == real(changed));

    // Libraries that link each other are each looked at once, and the search still ends where the fault lies.
    const auto first = rig.libraries / "First.dylib";
    const auto second = rig.libraries / "Second.dylib";
    files::writeSafely(first, machO({{loadDylib, paths::toUtf8(second)}, {loadDylib, paths::toUtf8(first)}}));
    files::writeSafely(second, machO({{loadDylib, paths::toUtf8(first)}, {loadDylib, name}}));
    CHECK(rig.linking({{loadDylib, paths::toUtf8(first)}}) == real(changed));
    files::writeSafely(second, machO({{loadDylib, paths::toUtf8(first)}}));
    CHECK(rig.linking({{loadDylib, paths::toUtf8(first)}}) == std::nullopt);

    // Not the system's, one that isn't there, or one found through the program rather than the plugin.
    CHECK(rig.linking({{loadDylib, "/usr/lib/libSystem.B.dylib"},
                       {loadDylib, "/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation"},
                       {loadDylib, "/nowhere/Missing.dylib"},
                       {loadDylib, "@executable_path/Changed.dylib"},
                       {loadDylib, "Changed.dylib"}}) == std::nullopt);
}

TEST_CASE("Only the code the system would run counts against a plugin")
{
    Rig rig;

    // A binary built for both architectures: the other's code never runs here.
    const auto bothAt = bundleOf(rig.plugins, "Both", "");
    std::filesystem::copy_file(library, binaryOf(bothAt), std::filesystem::copy_options::overwrite_existing);
    sign(binaryOf(bothAt));
    CHECK(rig.vst3.changedAfterSigning(bothAt) == std::nullopt);
    damage(binaryOf(bothAt), otherArchitecture());
    REQUIRE_FALSE(
        tool("codesign --verify --ignore-resources -a " + otherArchitecture() + " " + quoted(binaryOf(bothAt))));
    CHECK(rig.vst3.changedAfterSigning(bothAt) == std::nullopt);
    damage(binaryOf(bothAt));
    CHECK(rig.vst3.changedAfterSigning(bothAt) == real(binaryOf(bothAt)));

    // With no signature, nothing was changed after signing.
    const auto unsignedAt = bundleOf(rig.plugins, "Unsigned", "");
    REQUIRE(tool("lipo -thin " + std::string(system::architecture()) + " " + quoted(library) + " -output " +
                 quoted(binaryOf(unsignedAt))));
    REQUIRE(tool("codesign --remove-signature " + quoted(binaryOf(unsignedAt))));
    CHECK(rig.vst3.changedAfterSigning(unsignedAt) == std::nullopt);

    // A bundle signed whole whose other files changed since.
    const auto sealedBundle = [&](const std::string& name) {
        const auto bundle = bundleOf(rig.plugins, name, "");
        std::filesystem::copy_file(library, binaryOf(bundle), std::filesystem::copy_options::overwrite_existing);
        files::writeSafely(bundle / "Contents" / "Info.plist", infoNaming(name, "sealed"));
        std::filesystem::create_directories(bundle / "Contents" / "Resources");
        files::writeSafely(bundle / "Contents" / "Resources" / "One.txt", "one");
        sign(bundle);
        REQUIRE(tool("codesign --verify --strict " + quoted(bundle)));
        return bundle;
    };
    const auto sealed = sealedBundle("Sealed");
    files::writeSafely(sealed / "Contents" / "Resources" / "One.txt", "changed");
    files::writeSafely(sealed / "Contents" / "Resources" / "Two.txt", "added");
    REQUIRE_FALSE(tool("codesign --verify --strict " + quoted(sealed)));
    CHECK(rig.vst3.changedAfterSigning(sealed) == std::nullopt);
    damage(binaryOf(sealed));
    CHECK(rig.vst3.changedAfterSigning(sealed) == real(binaryOf(sealed)));

    const auto listed = sealedBundle("Listed");
    files::writeSafely(listed / "Contents" / "Info.plist", infoNaming("Listed", "changed"));
    REQUIRE_FALSE(tool("codesign --verify --strict " + quoted(listed)));
    CHECK(rig.vst3.changedAfterSigning(listed) == std::nullopt);
}

TEST_CASE("An Audio Unit built only for Intel is checked in its Intel code, which the system runs through Rosetta")
{
    if (system::architecture() != "arm64")
        SKIP("Rosetta runs Intel code on Apple Silicon only.");

    Rig rig;
    const auto unit = audioUnitOf(rig.plugins, "Intel", "x86_64");
    CHECK(rig.audioUnits.changedAfterSigning(unit) == std::nullopt);
    const auto binary = unit / "Contents" / "MacOS" / "Intel";
    damage(binary, "x86_64");
    CHECK(rig.audioUnits.changedAfterSigning(unit) == real(binary));

    const auto own = audioUnitOf(rig.plugins, "Own", "arm64");
    CHECK(rig.audioUnits.changedAfterSigning(own) == std::nullopt);
    damage(own / "Contents" / "MacOS" / "Own", "arm64");
    CHECK(rig.audioUnits.changedAfterSigning(own) == real(own / "Contents" / "MacOS" / "Own"));
}

TEST_CASE(
    "On Apple Silicon a walk gives a worker an Audio Unit built only for Intel, and lists a VST3 built only for it")
{
    if (system::architecture() != "arm64")
        SKIP("Rosetta runs Intel code on Apple Silicon only.");

    Rig rig;
    audioUnitOf(rig.plugins, "Intel Unit", "x86_64");
    const auto vst3 = bundleOf(rig.plugins, "Intel Module", "");
    REQUIRE(tool("lipo -thin x86_64 " + quoted(library) + " -output " + quoted(binaryOf(vst3))));

    PluginList list{{rig.scratch.path / "Settings" / "Plugins.json", "1.0", nullptr, nullptr, nullptr}};
    const auto units = PluginScanner::walk({{&rig.audioUnits, {rig.plugins, false}}}, list);
    REQUIRE(units.files.size() == 1);
    CHECK(units.files[0].architectures == std::vector<std::string>{"x86_64"});
    CHECK_FALSE(list.file(units.files[0].path).has_value());

    const auto modules = PluginScanner::walk({{&rig.vst3, {rig.plugins, false}}}, list);
    CHECK(modules.files.empty());
    const auto listed = list.file(real(vst3));
    REQUIRE(listed);
    CHECK(listed->outcome == PluginFile::Outcome::otherArchitecture);

    CHECK(rig.audioUnits.runs("x86_64"));
    CHECK(rig.audioUnits.runs("arm64"));
    CHECK_FALSE(rig.audioUnits.runs("x86"));
    CHECK(rig.vst3.runs("arm64"));
    CHECK_FALSE(rig.vst3.runs("x86_64"));
}

TEST_CASE("Damaged binaries end the check without harm, whatever their headers say")
{
    Rig rig;
    const auto bundle = bundleOf(rig.plugins, "Damaged", "");
    const auto file = binaryOf(bundle);
    const auto changed = rig.libraryNamed("Changed.dylib", true);
    const auto base = machO({{runPath, "@loader_path/../../../Libraries"},
                             {loadDylib, "@rpath/Whole.dylib"},
                             {weakDylib, "/nowhere/Missing.dylib"}});
    const auto cpu = little(cpuOf(system::architecture()), 4);
    const auto bigWord = [](std::uint32_t value) {
        return std::string{static_cast<char>(value >> 24), static_cast<char>(value >> 16),
                           static_cast<char>(value >> 8), static_cast<char>(value)};
    };

    int checked = 0;
    const auto check = [&](const std::string& bytes) {
        files::writeSafely(file, bytes);
        CHECK(rig.vst3.changedAfterSigning(bundle) == std::nullopt);
        ++checked;
    };

    // Each length it could be cut to.
    for (std::size_t size = 0; size <= base.size(); ++size)
        check(base.substr(0, size));

    // Bytes changed anywhere, with a fixed seed so a failure comes again.
    std::mt19937 random(20261030);
    for (int i = 0; i < 1000; ++i)
    {
        auto bytes = base;
        for (int changes = 0; changes < 3; ++changes)
            bytes[random() % bytes.size()] = static_cast<char>(random());
        check(bytes);
    }

    // Headers that promise what isn't there.
    const auto header = [&](std::uint32_t count, std::uint32_t size) {
        return little(0xFEEDFACF, 4) + cpu + little(0, 4) + little(8, 4) + little(count, 4) + little(size, 4) +
               little(0, 4) + little(0, 4);
    };
    check(header(0xFFFFFFFF, 0xFFFFFFFF));

    // A command of no length never moves on, for all the commands its header promises.
    const auto began = std::chrono::steady_clock::now();
    check(header(0xFFFFFFFF, 8) + little(loadDylib, 4) + little(0, 4));
    CHECK(std::chrono::steady_clock::now() - began < 5s);

    check(header(1000, 8) + little(loadDylib, 4) + little(0, 4));
    check(header(1, 16) + little(loadDylib, 4) + little(16, 4) + little(0xFFFFFFFF, 4) + little(0, 4));
    check(header(1, 8) + little(loadDylib, 4) + little(8, 4));
    check(header(1, 32) + little(loadDylib, 4) + little(32, 4) + little(24, 4) + std::string(12, '\0') + "unended!");
    check(bigWord(0xCAFEBABE) + bigWord(0xFFFFFFFF));
    check(bigWord(0xCAFEBABE) + bigWord(2) + bigWord(static_cast<std::uint32_t>(cpuOf(system::architecture()))) +
          bigWord(0) + bigWord(0xFFFFFFF0) + bigWord(16) + bigWord(14));
    check(bigWord(0xCAFEBABF) + bigWord(1) + bigWord(static_cast<std::uint32_t>(cpuOf(system::architecture()))) +
          bigWord(0) + bigWord(0xFFFFFFFF) + bigWord(0xFFFFFFFF));
    check(std::string(4096, '\0'));
    CHECK(checked > 1000);

    // A library linked is read as warily as the binary itself is.
    const auto wild = rig.libraries / "Wild.dylib";
    files::writeSafely(wild, bigWord(0xCAFEBABE) + bigWord(0xFFFFFFFF));
    check(machO({{loadDylib, paths::toUtf8(wild)}}));
    files::writeSafely(wild, header(1, 0xFFFFFFFF));
    check(machO({{loadDylib, paths::toUtf8(wild)}}));

    // And it still finds what is really there, with a name that runs to the very end of its command.
    const auto name = paths::toUtf8(changed);
    const auto commands = little(loadDylib, 4) + little(24 + name.size(), 4) + little(24, 4) + std::string(12, '\0') +
                          name + little(runPath, 4) + little(24, 4) + little(12, 4) + std::string(12, 'x');
    files::writeSafely(file, header(2, static_cast<std::uint32_t>(commands.size())) + commands);
    CHECK(rig.vst3.changedAfterSigning(bundle) == real(changed));
    files::writeSafely(file, machO({{loadDylib, name}}));
    CHECK(rig.vst3.changedAfterSigning(bundle) == real(changed));
}

namespace
{

constexpr auto downloaded = "0081;6ae4db99;Browser;";

void flag(const std::filesystem::path& file, const std::string& value)
{
    REQUIRE(tool("xattr -w com.apple.quarantine '" + value + "' " + quoted(file)));
}

bool flagged(const std::filesystem::path& file, const std::string& value)
{
    return tool("xattr -p com.apple.quarantine " + quoted(file) + " | grep -qxF '" + value + "'");
}

// A report as the system writes one: a line of its header, then the report, with the first image's address where the
// system stopped it, in the second image's range.
std::string reportOf(std::int64_t process, const std::string& space)
{
    return R"({"bug_type":"309","name":"worker"})"
           "\n"
           R"({"pid":)" +
           std::to_string(process) + R"(,"termination":{"flags":0,"code":2,"namespace":")" + space +
           R"(","indicator":"Invalid Page"},"exception":{"type":"EXC_BAD_ACCESS",)"
           R"("subtype":"UNKNOWN_0x32 at 0x0000000000002000"},"usedImages":[{"base":4096,"size":4096,)"
           R"("path":"/Other.dylib"},{"base":8192,"size":4096,"path":"/At fault.dylib"}]})";
}

// A worker scanning a test plugin that ends the way its name says, with crash reports in a folder of the test's own.
struct Stopping : Rig
{
    std::filesystem::path reports = scratch.path / "Reports";
    std::filesystem::path bundle;
    std::unique_ptr<PluginScanWorker> worker;
    std::vector<PluginFailure> failed;
    std::vector<std::optional<PluginDescription>> inHand;

    Stopping(const std::string& name, std::chrono::milliseconds wait)
    {
        std::filesystem::create_directories(reports);
        bundle = plugins / (name + ".vst3");
        std::filesystem::copy(bundles / "Good A.vst3", bundle, std::filesystem::copy_options::recursive);
        std::error_code error;
        worker = PluginScanWorker::start(
            {SYMBOLENGINE_SCAN_WORKER, {}, scratch.path / "worker.txt", 20s, main.post(), reports, wait}, error);
        REQUIRE(worker);
        worker->onFailed = [this](const PluginFailure& failure, const std::optional<PluginDescription>& plugin) {
            failed.push_back(failure);
            inHand.push_back(plugin);
        };
    }

    void report(const std::string& text, const std::string& name = "worker.ips")
    {
        files::writeSafely(reports / name, text);
    }

    void scan() { REQUIRE(worker->scan({"VST3", bundle, PluginSetup{48000.0, 256, false}, {}})); }

    // Scans and waits to be told how it failed, giving how long that took.
    std::chrono::steady_clock::duration fail()
    {
        const auto began = std::chrono::steady_clock::now();
        scan();
        REQUIRE(main.turnUntil([&] { return !failed.empty(); }, 30s));
        return std::chrono::steady_clock::now() - began;
    }
};

}

TEST_CASE("A worker the system kills for its code says so, from the system's crash report")
{
    SECTION("With its report there already")
    {
        Stopping rig("Killed when made", 5s);
        rig.report(reportOf(rig.worker->processId(), "CODESIGNING"));
        CHECK(rig.fail() < 4s);
        CHECK(rig.failed[0].kind == PluginFailure::Kind::codeSignature);
        CHECK(rig.failed[0].code == 9);
        CHECK(rig.failed[0].detail.find("/At fault.dylib") != std::string::npos);
        CHECK(rig.failed[0].detail.find("/Other.dylib") == std::string::npos);
        CHECK(rig.failed[0].step.find("making plugin") == 0);
        CHECK(rig.inHand[0].has_value());
    }
    SECTION("With its report coming a moment later")
    {
        Stopping rig("Killed when made", 10s);
        rig.scan();
        REQUIRE(rig.main.turnUntil([&] { return rig.worker->hasEnded(); }, 20s));
        CHECK(rig.failed.empty());
        rig.report(reportOf(rig.worker->processId(), "CODESIGNING"));
        REQUIRE(rig.main.turnUntil([&] { return !rig.failed.empty(); }, 8s));
        CHECK(rig.failed[0].kind == PluginFailure::Kind::codeSignature);
    }
    SECTION("With its report written in two goes, read once it is whole")
    {
        Stopping rig("Killed when made", 10s);
        const auto whole = reportOf(rig.worker->processId(), "CODESIGNING");
        rig.report(whole.substr(0, whole.size() / 2));
        rig.scan();
        REQUIRE(rig.main.turnUntil([&] { return rig.worker->hasEnded(); }, 20s));
        rig.main.turnUntil([] { return false; }, 300ms);
        CHECK(rig.failed.empty());
        rig.report(whole);
        REQUIRE(rig.main.turnUntil([&] { return !rig.failed.empty(); }, 8s));
        CHECK(rig.failed[0].kind == PluginFailure::Kind::codeSignature);
    }
    SECTION("A report that names no file of its code still says what stopped it")
    {
        Stopping rig("Killed when made", 5s);
        rig.report(R"({"bug_type":"309"})"
                   "\n"
                   R"({"pid":)" +
                   std::to_string(rig.worker->processId()) + R"(,"termination":{"namespace":"CODESIGNING"}})");
        rig.fail();
        CHECK(rig.failed[0].kind == PluginFailure::Kind::codeSignature);
    }
}

TEST_CASE("A worker killed for anything else, or with no report of its own, is told as before")
{
    const auto crashed = [](Stopping& rig) {
        CHECK(rig.failed[0].kind == PluginFailure::Kind::crashed);
        CHECK(rig.failed[0].code == 9);
    };
    SECTION("No report at all, once the wait is over")
    {
        Stopping rig("Killed when made", 400ms);
        CHECK(rig.fail() >= 400ms);
        crashed(rig);
    }
    SECTION("Another process's report")
    {
        Stopping rig("Killed when made", 400ms);
        rig.report(reportOf(rig.worker->processId() + 1, "CODESIGNING"));
        rig.fail();
        crashed(rig);
    }
    SECTION("Its report saying something else stopped it")
    {
        Stopping rig("Killed when made", 400ms);
        rig.report(reportOf(rig.worker->processId(), "SIGNAL"));
        rig.fail();
        crashed(rig);
    }
    SECTION("A report written before it started, of another process with its number")
    {
        Stopping rig("Killed when made", 400ms);
        rig.report(reportOf(rig.worker->processId(), "CODESIGNING"));
        std::filesystem::last_write_time(rig.reports / "worker.ips",
                                         std::filesystem::file_time_type::clock::now() - std::chrono::hours(1));
        rig.fail();
        crashed(rig);
    }
    SECTION("A file that isn't a report")
    {
        Stopping rig("Killed when made", 400ms);
        rig.report(reportOf(rig.worker->processId(), "CODESIGNING"), "worker.txt");
        rig.report("not a report at all", "other.ips");
        rig.report("", "empty.ips");
        rig.report("{}\n[1, 2]", "array.ips");
        rig.fail();
        crashed(rig);
    }
    SECTION("A worker that crashed isn't waited for, whatever is in the folder")
    {
        Stopping rig("Crash when made", 20s);
        rig.report(reportOf(rig.worker->processId(), "CODESIGNING"));
        CHECK(rig.fail() < 10s);
        CHECK(rig.failed[0].kind == PluginFailure::Kind::crashed);
        CHECK(rig.failed[0].code != 9);
    }
    SECTION("Closing the worker ends the wait, and tells nothing")
    {
        Stopping rig("Killed when made", 60s);
        rig.scan();
        REQUIRE(rig.main.turnUntil([&] { return rig.worker->hasEnded(); }, 20s));
        const auto began = std::chrono::steady_clock::now();
        rig.worker->close();
        CHECK(std::chrono::steady_clock::now() - began < 5s);
        rig.main.turnUntil([] { return false; }, 100ms);
        CHECK(rig.failed.empty());
    }
}

TEST_CASE("A file whose worker the system kills for its code is listed as changed after signing")
{
    Rig rig;
    const auto reports = rig.scratch.path / "Reports";
    std::filesystem::create_directories(reports);
    for (const auto& name : {"A good one", "B killed for its code when made", "C killed when made", "D good one",
                             "E many 3 with 2 killed for its code when made"})
        std::filesystem::copy(bundles / "Good A.vst3", rig.plugins / (std::string(name) + ".vst3"),
                              std::filesystem::copy_options::recursive);

    const auto settings = rig.scratch.path / "Settings" / "Plugins.json";
    PluginList list{{settings, "1.0", nullptr, nullptr, nullptr}};
    PluginScanner scanner(list, PluginScanner::Options{{SYMBOLENGINE_SCAN_WORKER,
                                                        {{"SYMBOLENGINE_TEST_REPORTS", paths::toUtf8(reports)}},
                                                        rig.scratch.path / "workers.txt",
                                                        20s,
                                                        rig.main.post(),
                                                        reports,
                                                        1s},
                                                       PluginSetup{48000.0, 256, false},
                                                       50,
                                                       std::int64_t(2) * 1024 * 1024 * 1024,
                                                       10s});
    std::optional<PluginScanEnd> end;
    scanner.onFinished = [&](const PluginScanEnd& how) { end = how; };
    const auto walked = PluginScanner::walk({{&rig.vst3, {rig.plugins, false}}}, list);
    REQUIRE(walked.files.size() == 5);
    REQUIRE(scanner.scan(walked.files));
    REQUIRE(rig.main.turnUntil([&] { return end.has_value(); }, 60s));

    const auto recorded = [&](const std::string& name) { return list.file(real(rig.plugins / (name + ".vst3"))); };
    CHECK(recorded("A good one")->outcome == PluginFile::Outcome::found);
    CHECK(recorded("D good one")->outcome == PluginFile::Outcome::found);

    const auto stopped = recorded("B killed for its code when made");
    REQUIRE(stopped);
    CHECK(stopped->outcome == PluginFile::Outcome::changedAfterSigning);
    CHECK(stopped->complete);
    CHECK(stopped->plugins.empty());
    CHECK(stopped->setAside.empty());
    REQUIRE(stopped->failure);
    CHECK(stopped->failure->kind == PluginFailure::Kind::codeSignature);
    CHECK(stopped->failure->detail.find("/At fault.dylib") != std::string::npos);
    CHECK_FALSE(list.needsScan(stopped->path, stopped->stamp));

    // What was found of a file before its code was stopped goes with it, as all of it is that code.
    const auto many = recorded("E many 3 with 2 killed for its code when made");
    REQUIRE(many);
    CHECK(many->outcome == PluginFile::Outcome::changedAfterSigning);
    CHECK(many->plugins.empty());
    CHECK(many->setAside.empty());

    // Each was in a worker that scanned a file before, so a fresh one was asked first: two reports each.
    int written = 0;
    for (const auto& entry : std::filesystem::directory_iterator(reports))
        written += entry.path().extension() == ".ips" ? 1 : 0;
    CHECK(written == 4);

    // One with no report is a crash of its plugin, as before.
    const auto crashed = recorded("C killed when made");
    REQUIRE(crashed);
    CHECK(crashed->outcome == PluginFile::Outcome::found);
    REQUIRE(crashed->setAside.size() == 1);
    CHECK(crashed->setAside[0].failure.kind == PluginFailure::Kind::crashed);

    // The list keeps it.
    std::error_code error;
    list.save(error);
    REQUIRE_FALSE(error);
    PluginList again{{settings, "1.0", nullptr, nullptr, nullptr}};
    const auto kept = again.file(stopped->path);
    REQUIRE(kept);
    CHECK(kept->outcome == PluginFile::Outcome::changedAfterSigning);
    REQUIRE(kept->failure);
    CHECK(kept->failure->kind == PluginFailure::Kind::codeSignature);
}

TEST_CASE("A fault the check can't see is stopped by the system, and its crash report says so")
{
    if (system::architecture() != "arm64")
        SKIP("Only Apple Silicon stops every process for this.");

    Rig rig;
    const auto bundle = rig.plugins / "Unseen.vst3";
    std::filesystem::copy(bundles / "Good A.vst3", bundle, std::filesystem::copy_options::recursive);
    sign(bundle);

    // Code changed after signing, in a bundle whose Info.plist changed too, which Apple's check stops at.
    const auto binary = bundle / "Contents" / "MacOS" / "SymbolEngine Misbehaving";
    damage(binary);
    std::error_code error;
    auto info = files::read(bundle / "Contents" / "Info.plist", error);
    REQUIRE_FALSE(error);
    files::writeSafely(bundle / "Contents" / "Info.plist", info + "\n");
    REQUIRE(rig.vst3.changedAfterSigning(bundle) == std::nullopt);

    // The worker under a name of its own, as the system stops writing reports of a name it has written many of, which
    // the other tests' crashes soon are.
    std::random_device random;
    const auto name = "symbolengine_unseen_" + std::to_string(random());
    const auto program = rig.scratch.path / name;
    std::filesystem::copy_file(SYMBOLENGINE_SCAN_WORKER, program);

    PluginList list{{rig.scratch.path / "Settings" / "Plugins.json", "1.0", nullptr, nullptr, nullptr}};
    PluginScanner scanner(list,
                          PluginScanner::Options{{program, {}, rig.scratch.path / "workers.txt", 20s, rig.main.post()},
                                                 PluginSetup{48000.0, 256, false},
                                                 50,
                                                 std::int64_t(2) * 1024 * 1024 * 1024,
                                                 10s});
    std::optional<PluginScanEnd> end;
    scanner.onFinished = [&](const PluginScanEnd& how) { end = how; };
    REQUIRE(scanner.scan(PluginScanner::walk({{&rig.vst3, {rig.plugins, false}}}, list).files));
    REQUIRE(rig.main.turnUntil([&] { return end.has_value(); }, 60s));

    // What the system wrote of this test's worker is the test's to take away.
    const auto folder = paths::systemFolder(paths::Folder::home) / "Library" / "Logs" / "DiagnosticReports";
    int removed = 0;
    for (const auto& entry : std::filesystem::directory_iterator(folder, error))
        if (paths::toUtf8(entry.path().filename()).starts_with(name))
        {
            std::filesystem::remove(entry.path(), error);
            ++removed;
        }

    const auto listed = list.file(real(bundle));
    REQUIRE(listed);
    INFO("reports of this test's workers: " << removed);
    INFO("outcome " << static_cast<int>(listed->outcome) << " plugins " << listed->plugins.size() << " aside "
                    << listed->setAside.size() << " detail "
                    << (listed->failure ? listed->failure->step + ": " + listed->failure->detail : std::string("none"))
                    << (listed->setAside.empty() ? std::string() : listed->setAside[0].failure.detail));
    CHECK(removed > 0);
    CHECK(listed->outcome == PluginFile::Outcome::changedAfterSigning);
    REQUIRE(listed->failure);
    CHECK(listed->failure->kind == PluginFailure::Kind::codeSignature);
    CHECK(listed->failure->detail.find("Unseen.vst3") != std::string::npos);
    CHECK(listed->plugins.empty());
}

TEST_CASE("Only code carrying the flag of a download, not yet allowed by the user, that the system rejects is refused")
{
    if (!tool("spctl --status | grep -q 'assessments enabled'"))
        SKIP("The system's assessments are switched off here, so it rejects nothing.");

    Rig rig;
    const auto bundle = bundleOf(rig.plugins, "Signed", "");
    const auto binary = binaryOf(bundle);
    REQUIRE(tool("lipo -thin " + std::string(system::architecture()) + " " + quoted(library) + " -output " +
                 quoted(binary)));
    sign(binary);

    // The system rejects it if asked, but runs it, as nothing marks it as downloaded.
    REQUIRE_FALSE(tool("spctl --assess --type install " + quoted(binary)));
    CHECK(rig.vst3.notNotarized(bundle) == std::nullopt);

    flag(binary, downloaded);
    CHECK(rig.vst3.notNotarized(bundle) == real(binary));
    CHECK(flagged(binary, downloaded));

    // One the system can't say no to, here one with no code it can make sense of, is left to load, as a notarized one
    // is.
    const auto unreadable = bundleOf(rig.plugins, "Unreadable", machO({}));
    flag(binaryOf(unreadable), downloaded);
    CHECK(rig.vst3.notNotarized(unreadable) == std::nullopt);

    // Allowed by the user, as the flag then says.
    const std::string allowed = "00c1;6ae4db99;Browser;";
    flag(binary, allowed);
    CHECK(rig.vst3.notNotarized(bundle) == std::nullopt);
    CHECK(flagged(binary, allowed));

    // A flag it can't read says nothing of the user allowing it.
    flag(binary, "not a flag");
    CHECK(rig.vst3.notNotarized(bundle) == real(binary));

    // The system looks at the code, not the bundle around it.
    REQUIRE(tool("xattr -d com.apple.quarantine " + quoted(binary)));
    flag(bundle, downloaded);
    CHECK(rig.vst3.notNotarized(bundle) == std::nullopt);

    // A library it links, flagged where the plugin isn't.
    const auto linked = rig.libraryNamed("Flagged.dylib", false);
    flag(linked, downloaded);
    const auto linking = bundleOf(rig.plugins, "Linking", machO({{loadDylib, paths::toUtf8(linked)}}));
    CHECK(rig.vst3.notNotarized(linking) == real(linked));

    // Audio Units alike.
    const auto unit = audioUnitOf(rig.plugins, "Unit", std::string(system::architecture()));
    CHECK(rig.audioUnits.notNotarized(unit) == std::nullopt);
    flag(unit / "Contents" / "MacOS" / "Unit", downloaded);
    CHECK(rig.audioUnits.notNotarized(unit) == real(unit / "Contents" / "MacOS" / "Unit"));
}

TEST_CASE("A downloaded plugin its maker hasn't had notarized is listed as that without its code loaded, and looked at "
          "again at every scan")
{
    if (!tool("spctl --status | grep -q 'assessments enabled'"))
        SKIP("The system's assessments are switched off here, so it rejects nothing.");

    // Code only for the other architecture, which the system can assess, so even a worker that wrongly loads it fails
    // at once, before any of it is mapped, with no window of the system's for it.
    Rig rig;
    const auto bundle = bundleOf(rig.plugins, "Downloaded", "");
    REQUIRE(tool("lipo -thin " + otherArchitecture() + " " + quoted(library) + " -output " + quoted(binaryOf(bundle))));
    sign(binaryOf(bundle));
    flag(binaryOf(bundle), downloaded);

    // Code changed after signing is that first, whatever else is true of it.
    const auto changed = rig.plugins / "Changed.vst3";
    std::filesystem::copy(bundles / "Good A.vst3", changed, std::filesystem::copy_options::recursive);
    sign(changed);
    const auto changedBinary = changed / "Contents" / "MacOS" / "SymbolEngine Misbehaving";
    damage(changedBinary);
    flag(changedBinary, downloaded);

    PluginList list{{rig.scratch.path / "Settings" / "Plugins.json", "1.0", nullptr, nullptr, nullptr}};
    PluginScanner scanner(
        list,
        PluginScanner::Options{{SYMBOLENGINE_SCAN_WORKER, {}, rig.scratch.path / "workers.txt", 20s, rig.main.post()},
                               PluginSetup{48000.0, 256, false},
                               50,
                               std::int64_t(2) * 1024 * 1024 * 1024,
                               10s});
    std::map<std::filesystem::path, std::vector<std::string>> steps;
    std::optional<PluginScanEnd> end;
    scanner.onProgress = [&](const std::filesystem::path& file, const std::string& step) {
        steps[file].push_back(step);
    };
    scanner.onFinished = [&](const PluginScanEnd& how) { end = how; };
    const auto scan = [&] {
        end.reset();

        // Asked for by hand, as a walk lists a VST3 built only for the other architecture without a worker.
        std::vector<PluginScanFile> files;
        for (const auto& file : {real(bundle), real(changed)})
        {
            PluginError problem;
            const auto facts = rig.vst3.examine(file, problem);
            REQUIRE(facts);
            files.push_back({"VST3", file, facts->stamp, false, facts->architectures});
        }
        REQUIRE(scanner.scan(files));
        REQUIRE(rig.main.turnUntil([&] { return end.has_value(); }, 60s));
    };
    scan();

    const auto listed = list.file(real(bundle));
    REQUIRE(listed);
    CHECK(listed->outcome == PluginFile::Outcome::notNotarized);
    CHECK(listed->complete);
    CHECK(listed->plugins.empty());
    REQUIRE(listed->failure);
    CHECK(listed->failure->kind == PluginFailure::Kind::refused);
    CHECK(listed->failure->detail.find(paths::toUtf8(real(binaryOf(bundle)))) != std::string::npos);
    const auto saw = steps[real(bundle)];
    CHECK(std::find(saw.begin(), saw.end(), "loading the code") == saw.end());
    CHECK(flagged(binaryOf(bundle), downloaded));

    CHECK(list.file(real(changed))->outcome == PluginFile::Outcome::changedAfterSigning);

    // The user's allowing it changes no stamp, so it is looked at again without being told.
    CHECK(list.needsScan(listed->path, listed->stamp));
    REQUIRE(tool("xattr -d com.apple.quarantine " + quoted(binaryOf(bundle))));
    scan();
    const auto again = list.file(real(bundle));
    REQUIRE(again);
    CHECK(again->outcome == PluginFile::Outcome::setAside);
    REQUIRE(again->failure);
    CHECK(again->failure->step == "loading the code");
}
