#include "TextValues.h"

#include <symbolengine/core/Json.h>
#include <symbolengine/plugin.h>

#include <symbolengine/core/Text.h>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string_view>

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

TEST_CASE("A scale spreads units over 0 to 1 evenly, or evenly by ratio")
{
    // What an equalizer released with these scales shows for positions its hosts recorded: 42.2 Hz, 1000 Hz and
    // 3.56 kHz from 20 Hz to 20 kHz, -4.2 dB from -30 to +30 dB, and Q 0.95 and 1.00 from 0.025 to 40.
    const auto frequency = Scale::ratio(20.0, 20000.0);
    CHECK(std::abs(frequency.toUnits(0.108153269) - 42.2) < 0.05);
    CHECK(std::abs(frequency.toUnits(0.566323) - 1000.0) < 0.05);
    CHECK(std::abs(frequency.toUnits(0.75) - 3560.0) < 5.0);
    CHECK(std::abs(frequency.toPosition(1000.0) - 0.566323) < 1e-6);
    CHECK(std::abs(frequency.toUnits(0.5) - std::sqrt(20.0 * 20000.0)) < 1e-9);
    const auto q = Scale::ratio(0.025, 40.0);
    CHECK(std::abs(q.toUnits(0.493687689) - 0.95) < 0.005);
    CHECK(std::abs(q.toUnits(0.5) - 1.0) < 1e-12);

    const auto gain = Scale::linear(-30.0, 30.0);
    CHECK(gain.toUnits(0.5) == 0.0);
    CHECK(std::abs(gain.toUnits(0.430611551) - -4.2) < 0.05);
    CHECK(gain.toPosition(-30.0) == 0.0);

    // Held to their ranges, and back again where they started.
    CHECK(frequency.toUnits(-1.0) == 20.0);
    CHECK(frequency.toUnits(2.0) == 20000.0);
    CHECK(frequency.toPosition(5.0) == 0.0);
    for (const double at : {0.0, 0.1, 0.25, 0.5, 0.75, 0.9, 1.0})
        CHECK(std::abs(frequency.toPosition(frequency.toUnits(at)) - at) < 1e-12);

    CHECK_THROWS_AS(Scale::ratio(0.0, 1.0), std::logic_error);
    CHECK_THROWS_AS(Scale::linear(1.0, 1.0), std::logic_error);
}

TEST_CASE("A scaled parameter is 0 to 1 to hosts and shown and typed in its units")
{
    const auto scale = Scale::ratio(20.0, 20000.0);
    const auto hertz = [](double units) { return text::formatFixed(units, 0) + " Hz"; };
    const auto kilohertz = [](std::string_view typed) -> std::optional<double> {
        const auto number = detail::leadingNumber(typed);
        if (number && typed.find('k') != std::string_view::npos)
            return *number * 1000.0;
        return number;
    };

    const auto frequency = scaled(7, "Frequency", scale, 1000.0, hertz, kilohertz);
    CHECK(frequency.minimum == 0.0);
    CHECK(frequency.maximum == 1.0);
    CHECK(std::abs(frequency.defaultValue - 0.566323) < 1e-6);
    CHECK(frequency.toText(frequency.defaultValue) == "1000 Hz");
    CHECK(std::abs(*frequency.fromText("2.5 kHz") - scale.toPosition(2500.0)) < 1e-12);
    CHECK(*frequency.fromText("100000 Hz") == 1.0);
    CHECK_FALSE(frequency.fromText("loud").has_value());

    // Without a reading of its own, the number typed, in the parameter's units.
    const auto plain = scaled(8, "Gain", Scale::linear(-30.0, 30.0), 0.0,
                              [](double units) { return text::formatFixed(units, 1) + " dB"; });
    CHECK(plain.defaultValue == 0.5);
    CHECK(*plain.fromText("-15 dB") == 0.25);
    CHECK_THROWS_AS(scaled(9, "Nothing", scale, 1.0, {}), std::logic_error);
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
