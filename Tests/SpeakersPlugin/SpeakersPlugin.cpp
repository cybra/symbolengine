// A plugin that marks each channel with the speaker it is for, adding the speaker's number plus one to it, in any
// standard layout with the same layout in and out. A host that gives each channel a value of its own can then tell
// that every channel reached the plugin as the speaker it is for and came back where it went in.

#include <symbolengine/plugin.h>

#include <algorithm>
#include <vector>

using namespace symbolengine;
using namespace symbolengine::plugin;

class SpeakersTest final : public Plugin
{
public:
    static Description describe()
    {
        return {.id = "test.symbolengine.speakers",
                .name = "Speakers Test",
                .vendor = "SymbolEngine",
                .version = "1.0.0",
                .features = {"audio-effect", "utility", "mono", "stereo", "surround"}};
    }

    // clap-validator 0.4.1 needs two parameters, as IdentityTest says.
    SpeakersTest()
    {
        addParameter(decibels(1, "Unused", -24.0, 24.0, 0.0));
        addParameter(bypass(2));
    }

    bool accepts(const Layouts& layouts) const override
    {
        const auto standard = ChannelLayout::standard();
        return layouts.inputs[0] == layouts.outputs[0] &&
               std::find(standard.begin(), standard.end(), layouts.outputs[0]) != standard.end();
    }

    void prepare(double, std::uint32_t) override
    {
        marks.clear();
        for (const auto speaker : layouts().outputs[0].speakers())
            marks.push_back(static_cast<float>(speaker) + 1.0f);
    }

    void process(Block& block) override
    {
        const auto in = block.input();
        const auto out = block.output();
        for (std::size_t channel = 0; channel < out.size() && channel < marks.size(); ++channel)
            for (std::uint32_t i = 0; i < block.frames(); ++i)
                out[channel][i] = in[channel][i] + marks[channel];
    }

private:
    std::vector<float> marks;
};

SYMBOLENGINE_PLUGINS(SpeakersTest)
