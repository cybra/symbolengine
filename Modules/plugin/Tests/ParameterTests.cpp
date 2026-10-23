#include "TextValues.h"

#include <symbolengine/core/Json.h>
#include <symbolengine/plugin.h>

#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <stdexcept>

using namespace symbolengine;
using namespace symbolengine::plugin;

namespace
{
// A plugin made directly, without a host, to try parameters on.
class Bare final : public Plugin
{
public:
    void process(Block&) override {}

    using Plugin::addParameter;
    using Plugin::loadParameters;
    using Plugin::saveParameters;
};
}

TEST_CASE("Typed numbers are read the way people type them")
{
    using detail::leadingNumber;
    CHECK(leadingNumber("-6") == -6.0);
    CHECK(leadingNumber("+3.5") == 3.5);
    CHECK(leadingNumber(".5") == 0.5);
    CHECK(leadingNumber("5.") == 5.0);
    CHECK(leadingNumber(" 007 ") == 7.0);
    CHECK(leadingNumber("-6 dB") == -6.0);
    CHECK(leadingNumber("250Hz") == 250.0);
    CHECK(leadingNumber("1e3x") == 1000.0);
    CHECK(leadingNumber("1e") == 1.0);
    CHECK(leadingNumber("-inf dB") == -std::numeric_limits<double>::infinity());
    CHECK(leadingNumber("Inf") == std::numeric_limits<double>::infinity());
    CHECK_FALSE(leadingNumber("").has_value());
    CHECK_FALSE(leadingNumber("-").has_value());
    CHECK_FALSE(leadingNumber("e5").has_value());
    CHECK_FALSE(leadingNumber(".").has_value());
    CHECK_FALSE(leadingNumber("dB").has_value());
}

TEST_CASE("Parameters of the usual kinds show and read their values")
{
    const auto level = decibels(1, "Level", -60.0, 12.0, 0.0);
    CHECK(level.toText(-6.04) == "-6.0 dB");
    CHECK(level.toText(-0.04) == "0.0 dB");
    CHECK(level.fromText("-12.5 dB") == -12.5);

    const auto frequency = number(2, "Frequency", 20.0, 20000.0, 1000.0, "Hz", 0);
    CHECK(frequency.toText(249.6) == "250 Hz");
    CHECK(number(3, "Amount", 0.0, 1.0, 0.5).toText(0.126) == "0.13");

    const auto shape = choice(4, "Shape", {"Bell", "Low Shelf", "High Shelf"}, 1);
    CHECK(shape.stepped);
    CHECK(shape.maximum == 2.0);
    CHECK(shape.defaultValue == 1.0);
    CHECK(shape.toText(2.0) == "High Shelf");
    CHECK(shape.toText(7.0) == "High Shelf");
    CHECK(shape.fromText("low shelf") == 1.0);
    CHECK(shape.fromText("2") == 2.0);
    CHECK_FALSE(shape.fromText("Notch").has_value());
    CHECK_THROWS_AS(choice(5, "None", {}, 0), std::logic_error);
    CHECK_THROWS_AS(choice(5, "Past", {"One"}, 1), std::logic_error);

    const auto power = toggle(6, "Power", true);
    CHECK(power.stepped);
    CHECK(power.defaultValue == 1.0);
    CHECK(power.toText(0.0) == "Off");
    CHECK(power.fromText("ON") == 1.0);
    CHECK(power.fromText("no") == 0.0);

    const auto off = bypass(7);
    CHECK(off.bypass);
    CHECK(off.stepped);
    CHECK(off.name == "Bypass");
}

TEST_CASE("A plugin refuses parameters it couldn't keep straight")
{
    Bare plugin;
    plugin.addParameter(decibels(1, "Gain", -60.0, 12.0, 0.0));
    CHECK_THROWS_AS(plugin.addParameter(decibels(1, "Again", -60.0, 12.0, 0.0)), std::logic_error);
    CHECK_THROWS_AS(plugin.addParameter(decibels(2, "Odd", -60.0, 12.0, 20.0)), std::logic_error);

    auto notStepped = bypass(3);
    notStepped.stepped = false;
    CHECK_THROWS_AS(plugin.addParameter(notStepped), std::logic_error);

    CHECK(plugin.parameters().size() == 1);
}

TEST_CASE("A parameter's value is read through what adding it returned")
{
    Bare plugin;
    const auto gain = plugin.addParameter(decibels(1, "Gain", -60.0, 12.0, -3.0));
    const auto mix = plugin.addParameter(number(2, "Mix", 0.0, 1.0, 1.0));
    CHECK(gain.id() == 1);
    CHECK(gain.get() == -3.0);
    CHECK(mix.get() == 1.0);
    CHECK(plugin.parameterValue(2) == 1.0);
    CHECK(plugin.parameterValue(9) == 0.0);
    CHECK(plugin.findParameter(2)->name == "Mix");
    CHECK(plugin.findParameter(9) == nullptr);

    plugin.loadParameters(json::parse(R"({"1": -12, "2": 5})"));
    CHECK(gain.get() == -12.0);
    CHECK(mix.get() == 1.0);   // held to its range
    CHECK(json::write(plugin.saveParameters()) == R"({"1":-12,"2":1})");

    CHECK(plugin.loadState(R"({"parameters": {"2": 0.5}})"));
    CHECK(gain.get() == -3.0);
    CHECK(mix.get() == 0.5);
    CHECK_FALSE(plugin.loadState("[]"));
}
