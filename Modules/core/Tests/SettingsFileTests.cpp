#include "Scratch.h"

#include <symbolengine/core/Files.h>
#include <symbolengine/core/SettingsFile.h>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <thread>
#include <vector>

using namespace symbolengine;

TEST_CASE("Settings start empty without a file, and make none until something changes")
{
    ScratchFolder scratch;
    const auto file = scratch.path / "settings.json";
    {
        SettingsFile settings(file);
        CHECK(settings.all().empty());
        CHECK(settings.get("anything").isNull());
        CHECK_FALSE(settings.hasChanges());
    }
    CHECK_FALSE(std::filesystem::exists(file));
}

TEST_CASE("Settings saved are there when the file is opened again")
{
    ScratchFolder scratch;
    const auto file = scratch.path / "Symbol" / "settings.json";
    {
        SettingsFile settings(file);
        settings.set("rate", 48000);
        settings.set("device", "Studio Interface");
        settings.set("window", Object{{"x", 10}, {"y", 20}});
        CHECK(settings.hasChanges());
        settings.save();
        CHECK_FALSE(settings.hasChanges());
    }

    CHECK(files::read(file) == "{\n  \"rate\": 48000,\n  \"device\": \"Studio Interface\",\n"
                               "  \"window\": {\n    \"x\": 10,\n    \"y\": 20\n  }\n}\n");

    SettingsFile again(file);
    CHECK(again.get("rate").asInt() == 48000);
    CHECK(again.get("device").asString() == "Studio Interface");
    CHECK(again.get("window")["y"].asInt() == 20);
}

TEST_CASE("Settings are saved when they are let go")
{
    ScratchFolder scratch;
    const auto file = scratch.path / "settings.json";
    {
        SettingsFile settings(file);
        settings.set("theme", "dark");
    }
    CHECK(SettingsFile(file).get("theme").asString() == "dark");
}

TEST_CASE("Setting the value a setting already has changes nothing")
{
    ScratchFolder scratch;
    SettingsFile settings(scratch.path / "settings.json");
    settings.set("gain", 1);
    settings.save();

    settings.set("gain", 1);
    CHECK_FALSE(settings.hasChanges());
    settings.set("gain", 2);
    CHECK(settings.hasChanges());

    CHECK(settings.remove("gain"));
    CHECK_FALSE(settings.remove("gain"));
    CHECK(settings.get("gain").isNull());
}

TEST_CASE("A settings file that can't be read is kept aside, not overwritten")
{
    ScratchFolder scratch;
    const auto file = scratch.path / "settings.json";

    for (const char* unreadable : {"{\"rate\": 480", "[1, 2, 3]", "\xFF\xFE garbage"})
    {
        files::writeSafely(file, unreadable);
        {
            SettingsFile settings(file);
            CHECK(settings.all().empty());
            CHECK(files::read(scratch.path / "settings.json.unreadable") == unreadable);
            settings.set("fresh", true);
        }
        CHECK(SettingsFile(file).get("fresh").asBool());
    }
}

TEST_CASE("A settings file that exists but couldn't be read is left as it was, and isn't saved over")
{
    ScratchFolder scratch;
    const auto file = scratch.path / "settings.json";

    // A folder by that name can be read nowhere, on any system.
    std::filesystem::create_directories(file);
    {
        SettingsFile settings(file);
        CHECK(settings.all().empty());
        CHECK(settings.readProblem());
        settings.set("fresh", true);
        std::error_code error;
        settings.save(error);
        CHECK(error);
        CHECK_THROWS_AS(settings.save(), std::filesystem::filesystem_error);
    }
    CHECK(std::filesystem::is_directory(file));
    CHECK_FALSE(std::filesystem::exists(scratch.path / "settings.json.unreadable"));
    std::filesystem::remove(file);

#if !defined(_WIN32)
    // Good settings the program may not read for the moment, as when it can't open another file.
    files::writeSafely(file, "{\"rate\": 48000}");
    std::filesystem::permissions(file, std::filesystem::perms::none);
    {
        SettingsFile settings(file);
        CHECK(settings.get("rate").isNull());
        CHECK(settings.readProblem());
        settings.set("rate", 44100);
    }
    CHECK_FALSE(std::filesystem::exists(scratch.path / "settings.json.unreadable"));
    std::filesystem::permissions(file, std::filesystem::perms::owner_all);
    SettingsFile again(file);
    CHECK_FALSE(again.readProblem());
    CHECK(again.get("rate").intOr(0) == 48000);
#endif

    // Neither a missing file nor one moved aside is a problem to the program.
    SettingsFile missing(scratch.path / "missing.json");
    CHECK_FALSE(missing.readProblem());
    files::writeSafely(scratch.path / "bad.json", "[1, 2, 3]");
    SettingsFile bad(scratch.path / "bad.json");
    CHECK_FALSE(bad.readProblem());
}

TEST_CASE("Settings can be changed from several threads at once")
{
    ScratchFolder scratch;
    SettingsFile settings(scratch.path / "settings.json");

    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
    {
        threads.emplace_back([&settings, t] {
            for (int i = 0; i < 500; ++i)
            {
                settings.set("thread" + std::to_string(t) + "." + std::to_string(i), i);
                if (i % 100 == 0)
                    settings.save();
            }
        });
    }
    for (auto& thread : threads)
        thread.join();

    settings.save();
    const auto all = SettingsFile(scratch.path / "settings.json").all();
    CHECK(all.size() == 2000);
    CHECK(*all.find("thread3.499") == Value(499));
}
