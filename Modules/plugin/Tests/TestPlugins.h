#pragma once

// The plugins the plugin module's tests make, listed in ClapTests.cpp's SYMBOLENGINE_PLUGINS.

#include <symbolengine/plugin.h>

#include <array>
#include <cmath>
#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

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

// A resizable editor's page: it asks for a size too small, then for a larger one, and reports what it got.
inline std::optional<std::string_view> resizePage(std::string_view path)
{
    static const std::string page = R"html(<!doctype html>
<meta charset="utf-8">
<script src="symbolengine.js"></script>
<script src="plugin.js"></script>
<script>
(async () => {
    await symbolengine.call("report", "tooSmall", await plugin.resize(10, 10));
    await symbolengine.call("report", "larger", await plugin.resize(500, 320));

    // The grip, dragged 100 points right and 50 down.
    const grip = await new Promise(found => {
        const look = () => document.querySelector(".symbolengine-grip") ? found(document.querySelector(".symbolengine-grip"))
                                                                          : setTimeout(look, 10);
        look();
    });
    const at = (x, y) => ({ pointerId: 1, screenX: x, screenY: y, bubbles: true });
    grip.dispatchEvent(new PointerEvent("pointerdown", at(0, 0)));
    await new Promise(wait => setTimeout(wait, 100));
    grip.dispatchEvent(new PointerEvent("pointermove", at(100, 50)));
    grip.dispatchEvent(new PointerEvent("pointerup", at(100, 50)));
    await new Promise(wait => setTimeout(wait, 200));
    await symbolengine.call("done");
})();
</script>)html";
    return path == "/index.html" ? std::optional<std::string_view>(page) : std::nullopt;
}

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

    std::optional<Editor> editor() const override
    {
        return Editor{.width = 400,
                      .height = 240,
                      .resizable = true,
                      .minimumWidth = 300,
                      .minimumHeight = 200,
                      .maximumWidth = 900,
                      .maximumHeight = 600,
                      .sizeFile = sizeFile,
                      .files = resizePage};
    }

    void editorOpened(EditorLink& link) override
    {
        if (onEditorOpened)
            onEditorOpened(link);
    }

    void editorResized(std::uint32_t width, std::uint32_t height) override { resized = {width, height}; }

    using Plugin::setEditorSize;

    static inline std::filesystem::path sizeFile;
    static inline std::function<void(EditorLink&)> onEditorOpened;
    std::pair<std::uint32_t, std::uint32_t> resized{};
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
