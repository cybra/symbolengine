// The smallest useful plugin: a gain, in decibels.

#include <symbolengine/plugin.h>

#include <cmath>

using namespace symbolengine::plugin;

class Gain final : public Plugin
{
public:
    static Description describe()
    {
        return {.id = "dev.symbolengine.examples.gain",
                .name = "Gain",
                .vendor = "SymbolEngine",
                .version = "0.1.0",
                .description = "Raises or lowers the level.",
                .url = "https://symbolengine.dev",
                .features = {"audio-effect", "utility", "stereo"}};
    }

    Gain() : gain(addParameter(decibels(1, "Gain", -60.0, 24.0, 0.0))) {}

    void process(Block& block) override
    {
        const auto factor = static_cast<float>(std::pow(10.0, gain.get() / 20.0));
        const auto in = block.input();
        const auto out = block.output();

        for (std::size_t channel = 0; channel < out.size(); ++channel)
            for (std::uint32_t i = 0; i < block.frames(); ++i)
                out[channel][i] = in[channel][i] * factor;
    }

private:
    ParameterValue gain;
};

SYMBOLENGINE_PLUGINS(Gain)
