#include "Identity.h"
#include "TestHost.h"
#include "TestPlugins.h"

#include <symbolengine/core/Text.h>
#include <symbolengine/plugin.h>

#include <clapwrapper/vst3.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

using namespace symbolengine;
using namespace symbolengine::plugin;

namespace
{
const double halfGain = 20.0 * std::log10(0.5);

using tests::TestGain;
using tests::TestLatency;

const clap_plugin_params& paramsOf(const Made& made)
{
    return made.extension<clap_plugin_params>(CLAP_EXT_PARAMS);
}

double valueOf(const Made& made, clap_id id)
{
    double value = std::numeric_limits<double>::quiet_NaN();
    REQUIRE(paramsOf(made).get_value(made.plugin, id, &value));
    return value;
}

void start(const Made& made)
{
    REQUIRE(made.plugin->activate(made.plugin, 48000.0, 1, 512));
    REQUIRE(made.plugin->start_processing(made.plugin));
}
}

SYMBOLENGINE_PLUGINS(TestGain, TestLatency)

TEST_CASE("The factory describes every plugin the binary offers")
{
    const auto& f = factory();
    REQUIRE(f.get_plugin_count(&f) == 2);

    const auto* first = f.get_plugin_descriptor(&f, 0);
    REQUIRE(first != nullptr);
    CHECK(std::string(first->id) == "test.gain");
    CHECK(std::string(first->name) == "Test Gain");
    CHECK(std::string(first->vendor) == "Tests");
    CHECK(std::string(first->version) == "1.0.0");
    CHECK(std::string(first->description) == "A gain for the tests.");
    CHECK(std::string(first->url).empty());
    CHECK(std::string(first->features[0]) == "audio-effect");
    CHECK(std::string(first->features[1]) == "stereo");
    CHECK(first->features[2] == nullptr);
    CHECK(clap_version_is_compatible(first->clap_version));

    CHECK(std::string(f.get_plugin_descriptor(&f, 1)->id) == "test.latency");
    CHECK(f.get_plugin_descriptor(&f, 1)->features[0] == nullptr);
    CHECK(f.get_plugin_descriptor(&f, 2) == nullptr);

    TestHost host;
    CHECK(f.create_plugin(&f, &host.host, "test.missing") == nullptr);

    host.host.clap_version = clap_version{0, 9, 0};
    CHECK(f.create_plugin(&f, &host.host, "test.gain") == nullptr);
}

TEST_CASE("A plugin made by a host processes its audio")
{
    TestHost host;
    Made made(host, "test.gain");
    REQUIRE(made.plugin != nullptr);
    start(made);
    CHECK(TestGain::last->preparedRate == 48000.0);

    Stereo audio(64);
    CHECK(audio.run(made.plugin, Changes()) == CLAP_PROCESS_CONTINUE);
    CHECK(audio.out[0] == audio.in[0]);
    CHECK(audio.out[1] == audio.in[1]);

    Changes changes;
    changes.add(0, 1, halfGain);
    audio.run(made.plugin, changes);
    for (std::size_t i = 0; i < 64; ++i)
        CHECK(audio.out[1][i] == Catch::Approx(audio.in[1][i] * 0.5f).epsilon(1e-6));
}

TEST_CASE("A parameter change applies from the sample it arrives at")
{
    TestHost host;
    Made made(host, "test.gain");
    start(made);

    Stereo audio(64);
    Changes changes;
    changes.add(10, 1, halfGain).add(40, 1, 0.0);
    audio.run(made.plugin, changes);

    for (std::size_t i = 0; i < 64; ++i)
    {
        const float expected = i >= 10 && i < 40 ? audio.in[0][i] * 0.5f : audio.in[0][i];
        INFO("sample " << i);
        CHECK(audio.out[0][i] == Catch::Approx(expected).epsilon(1e-6));
    }

    const auto& plugin = *TestGain::last;
    REQUIRE(plugin.blocks == 3);
    CHECK(plugin.blockSizes[0] == 10);
    CHECK(plugin.blockSizes[1] == 30);
    CHECK(plugin.blockSizes[2] == 24);
}

TEST_CASE("A change finds its parameter by the cookie the host was given, or by its id")
{
    TestHost host;
    Made made(host, "test.gain");
    start(made);

    clap_param_info info{};
    REQUIRE(paramsOf(made).get_info(made.plugin, 0, &info));
    REQUIRE(info.cookie != nullptr);

    clap_param_info modeInfo{};
    REQUIRE(paramsOf(made).get_info(made.plugin, 1, &modeInfo));

    Stereo audio(8);
    Changes changes;
    changes.add(0, 1, -12.0, info.cookie).add(0, 3, 0.0).add(0, 4, 0.5, modeInfo.cookie);
    audio.run(made.plugin, changes);

    CHECK(valueOf(made, 1) == -12.0);
    CHECK(valueOf(made, 3) == 0.0);
    CHECK(valueOf(made, 4) == 0.5);   // the cookie belonged to another parameter, so the id decided
    CHECK(valueOf(made, 2) == 0.0);
}

TEST_CASE("Values a host sends are held to what the parameter allows")
{
    TestHost host;
    Made made(host, "test.gain");
    start(made);

    Stereo audio(8);
    Changes changes;
    changes.add(0, 1, 100.0)
        .add(0, 2, 0.7)
        .add(0, 4, std::numeric_limits<double>::quiet_NaN())
        .add(0, 99, 1.0)
        .add(0, 3, 0.0, nullptr, 5);   // aimed at one note, which these parameters don't have
    audio.run(made.plugin, changes);

    CHECK(valueOf(made, 1) == 12.0);
    CHECK(valueOf(made, 2) == 1.0);
    CHECK(valueOf(made, 4) == 0.25);
    CHECK(valueOf(made, 3) == 1.0);
    double unused = 0;
    CHECK_FALSE(paramsOf(made).get_value(made.plugin, 99, &unused));
}

TEST_CASE("Changes flushed while the plugin isn't processing take effect")
{
    TestHost host;
    Made made(host, "test.gain");

    Changes changes;
    changes.add(0, 1, -3.0).add(0, 2, 1.0);
    paramsOf(made).flush(made.plugin, &changes.list, nullptr);

    CHECK(valueOf(made, 1) == -3.0);
    CHECK(valueOf(made, 2) == 1.0);
}

TEST_CASE("Parameters are described to the host as they were declared")
{
    TestHost host;
    Made made(host, "test.gain");
    const auto& params = paramsOf(made);

    REQUIRE(params.count(made.plugin) == 4);

    clap_param_info info{};
    REQUIRE(params.get_info(made.plugin, 3, &info));
    CHECK(info.id == 4);
    CHECK(std::string(info.name) == "Plain");
    CHECK(std::string(info.module) == "Advanced/Hidden things");
    CHECK(info.min_value == -1.0);
    CHECK(info.max_value == 1.0);
    CHECK(info.default_value == 0.25);
    CHECK(info.flags == CLAP_PARAM_IS_AUTOMATABLE);

    REQUIRE(params.get_info(made.plugin, 1, &info));
    CHECK(info.flags == (CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_IS_STEPPED));
    CHECK_FALSE(params.get_info(made.plugin, 4, &info));

    char text[64];
    REQUIRE(params.value_to_text(made.plugin, 1, -6.0, text, sizeof text));
    CHECK(std::string(text) == "-6.0 dB");
    REQUIRE(params.value_to_text(made.plugin, 2, 1.0, text, sizeof text));
    CHECK(std::string(text) == "Wide");
    REQUIRE(params.value_to_text(made.plugin, 3, 0.0, text, sizeof text));
    CHECK(std::string(text) == "Off");
    REQUIRE(params.value_to_text(made.plugin, 4, 0.256, text, sizeof text));
    CHECK(std::string(text) == "0.26");
    CHECK_FALSE(params.value_to_text(made.plugin, 99, 0.0, text, sizeof text));

    double value = 0;
    REQUIRE(params.text_to_value(made.plugin, 2, "wide", &value));
    CHECK(value == 1.0);
    REQUIRE(params.text_to_value(made.plugin, 1, " -12 dB", &value));
    CHECK(value == -12.0);
    REQUIRE(params.text_to_value(made.plugin, 1, "1000", &value));
    CHECK(value == 12.0);
    REQUIRE(params.text_to_value(made.plugin, 1, "-inf", &value));
    CHECK(value == -60.0);
    CHECK_FALSE(params.text_to_value(made.plugin, 1, "loud", &value));
}

TEST_CASE("A name longer than the host's field is cut between characters")
{
    TestHost host;
    Made made(host, "test.latency");

    clap_param_info info{};
    REQUIRE(paramsOf(made).get_info(made.plugin, 0, &info));
    const std::string name(info.name);
    CHECK(name.size() < CLAP_NAME_SIZE);
    CHECK(name.size() >= CLAP_NAME_SIZE - 2);
    CHECK(text::isValidUtf8(name));
}

TEST_CASE("State a plugin saves is the state it loads")
{
    TestHost host;
    Bytes saved;
    {
        Made made(host, "test.gain");
        Changes changes;
        changes.add(0, 1, -9.5).add(0, 2, 1.0).add(0, 3, 0.0).add(0, 4, -0.75);
        paramsOf(made).flush(made.plugin, &changes.list, nullptr);
        REQUIRE(made.extension<clap_plugin_state>(CLAP_EXT_STATE).save(made.plugin, &saved.writer));
    }
    CHECK(saved.bytes == R"({"parameters":{"1":-9.5,"2":1,"3":0,"4":-0.75}})");

    Made again(host, "test.gain");
    REQUIRE(again.extension<clap_plugin_state>(CLAP_EXT_STATE).load(again.plugin, &saved.reader));
    CHECK(valueOf(again, 1) == -9.5);
    CHECK(valueOf(again, 2) == 1.0);
    CHECK(valueOf(again, 3) == 0.0);
    CHECK(valueOf(again, 4) == -0.75);
    REQUIRE(host.rescans.size() == 1);
    CHECK(host.rescans[0] == CLAP_PARAM_RESCAN_VALUES);
}

TEST_CASE("State that can't be read is refused, and missing values take their defaults")
{
    TestHost host;
    Made made(host, "test.gain");
    const auto& state = made.extension<clap_plugin_state>(CLAP_EXT_STATE);

    Changes changes;
    changes.add(0, 1, -20.0).add(0, 4, 1.0);
    paramsOf(made).flush(made.plugin, &changes.list, nullptr);

    Bytes broken;
    broken.bytes = "not a saved state";
    CHECK_FALSE(state.load(made.plugin, &broken.reader));
    CHECK(valueOf(made, 1) == -20.0);
    CHECK(host.rescans.empty());

    // As an earlier version without the fourth parameter would have saved it.
    Bytes older;
    older.bytes = R"({"parameters":{"1":-1.5,"2":0,"3":1}})";
    REQUIRE(state.load(made.plugin, &older.reader));
    CHECK(valueOf(made, 1) == -1.5);
    CHECK(valueOf(made, 4) == 0.25);
}

TEST_CASE("Ports are stereo in and stereo out, which may share memory")
{
    TestHost host;
    Made made(host, "test.gain");
    const auto& ports = made.extension<clap_plugin_audio_ports>(CLAP_EXT_AUDIO_PORTS);

    CHECK(ports.count(made.plugin, true) == 1);
    CHECK(ports.count(made.plugin, false) == 1);

    clap_audio_port_info info{};
    REQUIRE(ports.get(made.plugin, 0, true, &info));
    CHECK(info.id == 0);
    CHECK(std::string(info.name) == "Input");
    CHECK(info.flags == CLAP_AUDIO_PORT_IS_MAIN);
    CHECK(info.channel_count == 2);
    CHECK(std::string(info.port_type) == CLAP_PORT_STEREO);
    CHECK(info.in_place_pair == 0);
    CHECK_FALSE(ports.get(made.plugin, 1, false, &info));
}

TEST_CASE("Audio buffers that don't match the ports are refused")
{
    TestHost host;
    Made made(host, "test.gain");
    start(made);

    Stereo audio(16);
    audio.input.channel_count = 1;
    CHECK(audio.run(made.plugin, Changes()) == CLAP_PROCESS_ERROR);
}

TEST_CASE("A new latency is reported when the host activates the plugin again")
{
    TestHost host;
    Made made(host, "test.latency");
    const auto& latency = made.extension<clap_plugin_latency>(CLAP_EXT_LATENCY);
    start(made);
    CHECK(latency.get(made.plugin) == 32);
    CHECK(host.latencyChanges == 0);

    TestLatency::last->change(64);
    CHECK(host.restarts == 1);

    made.plugin->stop_processing(made.plugin);
    made.plugin->deactivate(made.plugin);
    start(made);
    CHECK(host.latencyChanges == 1);
    CHECK(latency.get(made.plugin) == 64);
    CHECK(made.extension<clap_plugin_tail>(CLAP_EXT_TAIL).get(made.plugin) == 0);
}

TEST_CASE("Denormal numbers are flushed to zero while a plugin processes")
{
    // Outside processing they aren't, so this test can fail.
    volatile float tiny = 1e-39f;
    REQUIRE(tiny * 0.5f != 0.0f);

    TestHost host;
    Made made(host, "test.gain");
    start(made);
    Stereo audio(8);
    audio.run(made.plugin, Changes());
    CHECK(TestGain::last->flushedDenormals);

    CHECK(tiny * 0.5f != 0.0f);
}

TEST_CASE("A plugin that throws while preparing refuses activation instead of crashing the host")
{
    TestHost host;
    Made made(host, "test.gain");
    TestGain::failToPrepare = true;
    CHECK_FALSE(made.plugin->activate(made.plugin, 44100.0, 1, 256));
    TestGain::failToPrepare = false;
    CHECK(made.plugin->activate(made.plugin, 44100.0, 1, 256));
}

TEST_CASE("A VST3 class ID is read as Steinberg's validator prints it")
{
    const auto id = detail::vst3ComponentId("ABCDEF01-9182-FAEB-5379-6D6244796E61");
    REQUIRE(id);
    const array_of_16_bytes expected = COMPONENT_ID(0xABCDEF01, 0x9182FAEB, 0x53796D62, 0x44796E61);
    CHECK(std::memcmp(id->data(), expected, 16) == 0);

    CHECK(detail::vst3ComponentId("{abcdef019182faeb53796d6244796e61}") == id);
    CHECK_FALSE(detail::vst3ComponentId("ABCDEF01").has_value());
    CHECK_FALSE(detail::vst3ComponentId("ABCDEF019182FAEB53796D6244796E61AA").has_value());
    CHECK_FALSE(detail::vst3ComponentId("GBCDEF019182FAEB53796D6244796E61").has_value());
}

TEST_CASE("Each plugin's VST3 build is given the class ID it states, or none to be made from its id")
{
    const auto* vst3 =
        static_cast<const clap_plugin_factory_as_vst3*>(symbolengine_clap_get_factory(CLAP_PLUGIN_FACTORY_INFO_VST3));
    REQUIRE(vst3 != nullptr);
    CHECK(symbolengine_clap_get_factory(CLAP_PLUGIN_FACTORY_INFO_VST3_V1) == vst3);

    const auto* gain = vst3->get_vst3_info(vst3, 0);
    REQUIRE(gain != nullptr);
    CHECK(gain->componentId == nullptr);

    const auto* latency = vst3->get_vst3_info(vst3, 1);
    REQUIRE(latency != nullptr);
    REQUIRE(latency->componentId != nullptr);
    const array_of_16_bytes expected = COMPONENT_ID(0xABCDEF01, 0x53796D62, 0x54657374, 0x4C617421);
    CHECK(std::memcmp(*latency->componentId, expected, 16) == 0);

    CHECK(vst3->get_vst3_info(vst3, 2) == nullptr);
}
