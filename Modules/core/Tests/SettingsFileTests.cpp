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
