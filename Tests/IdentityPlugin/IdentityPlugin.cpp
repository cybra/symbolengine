// A plugin with a trim and a bypass that states its own VST3 class ID.

#include <symbolengine/plugin.h>

#include <cmath>

using namespace symbolengine::plugin;

class IdentityTest final : public Plugin
{
public:
    static Description describe()
    {
        return {.id = "test.symbolengine.identity",
                .name = "Identity Test",
                .vendor = "SymbolEngine",
                .version = "1.0.0",
                .features = {"audio-effect", "utility", "stereo"},
                .vst3ClassId = "ABCDEF0153796D624964656E74697479"};
    }

    // clap-validator 0.4.1 divides by zero on a plugin with no parameters, and when a plugin's only parameter is a
    // switch it can draw the value the switch already has and report that nothing changed; so there are two.
    IdentityTest() : trim(addParameter(decibels(1, "Trim", -24.0, 24.0, 0.0))), off(addParameter(bypass(2))) {}

    void process(Block& block) override
    {
        const auto factor = off.get() >= 0.5 ? 1.0f : static_cast<float>(std::pow(10.0, trim.get() / 20.0));
        for (std::size_t channel = 0; channel < block.output().size(); ++channel)
            for (std::uint32_t i = 0; i < block.frames(); ++i)
                block.output()[channel][i] = block.input()[channel][i] * factor;
    }

private:
    ParameterValue trim, off;
};

SYMBOLENGINE_PLUGINS(IdentityTest)
