#pragma once

// The plugins the plugin module's tests make, listed in ClapTests.cpp's SYMBOLENGINE_PLUGINS.

#include <symbolengine/plugin.h>

#include <array>
#include <cmath>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

// The editor page the tests use, built in from Tests/EditorPage.
std::optional<std::string_view> testEditorFiles(std::string_view path);

namespace tests
{

using namespace symbolengine::plugin;

class TestGain final : public Plugin
{
public:
    static Description describe()
    {
        return {.id = "test.gain",
                .name = "Test Gain",
                .vendor = "Tests",
                .version = "1.0.0",
                .description = "A gain for the tests.",
                .features = {"audio-effect", "stereo"}};
    }

    TestGain()
        : gain(addParameter(decibels(1, "Gain", -60.0, 12.0, 0.0))),
          mode(addParameter(choice(2, "Mode", {"Narrow", "Wide"}, 0))), on(addParameter(toggle(3, "On", true)))
    {
        addParameter(Parameter{.id = 4,
                               .name = "Plain",
                               .group = "Advanced/Hidden things",
                               .minimum = -1.0,
                               .maximum = 1.0,
                               .defaultValue = 0.25});
        last = this;
    }

    ~TestGain() override
    {
        if (last == this)
            last = nullptr;
    }

    void prepare(double sampleRate, std::uint32_t) override
    {
        if (failToPrepare)
            throw std::runtime_error("couldn't prepare");
        preparedRate = sampleRate;
    }

    void process(Block& block) override
    {
        if (blocks < blockSizes.size())
            blockSizes[blocks++] = block.frames();

        volatile float tiny = 1e-39f;
        flushedDenormals = tiny * 0.5f == 0.0f;

        const auto factor = static_cast<float>(std::pow(10.0, gain.get() / 20.0));
        for (std::size_t channel = 0; channel < block.output().size(); ++channel)
            for (std::uint32_t i = 0; i < block.frames(); ++i)
                block.output()[channel][i] = block.input()[channel][i] * factor;
    }

    std::optional<Editor> editor() const override
    {
        return Editor{.width = 320, .height = 200, .files = testEditorFiles};
    }

    void editorOpened(EditorLink& link) override
    {
        if (onEditorOpened)
            onEditorOpened(link);
    }

    static inline TestGain* last = nullptr;
    static inline bool failToPrepare = false;
    static inline std::function<void(EditorLink&)> onEditorOpened;

    ParameterValue gain, mode, on;
    double preparedRate = 0.0;
    std::array<std::uint32_t, 16> blockSizes{};
    std::size_t blocks = 0;
    bool flushedDenormals = false;
};

class TestLatency final : public Plugin
{
public:
    static Description describe()
    {
        return {.id = "test.latency",
                .name = "Test Latency",
                .vendor = "Tests",
                .version = "2.0.0",
                .vst3ClassId = "ABCDEF0153796D62546573744C617421"};
    }

    TestLatency()
    {
        // 200 two-byte characters: longer than any host's field for a name.
        std::string longName;
        for (int i = 0; i < 200; ++i)
            longName += "\xC3\xA9";
        addParameter(Parameter{.id = 7, .name = longName});
        last = this;
    }

    ~TestLatency() override
    {
        if (last == this)
            last = nullptr;
    }

    std::uint32_t latency() const override { return frames; }
    void process(Block&) override {}

    void change(std::uint32_t newFrames)
    {
        frames = newFrames;
        latencyChanged();
    }

    static inline TestLatency* last = nullptr;
    std::uint32_t frames = 32;
};
}
