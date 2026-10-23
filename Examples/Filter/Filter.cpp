// A filter with an editor: low pass, high pass or band pass, its cutoff and resonance on knobs, and the output's level
// on a meter.

#include <symbolengine/plugin.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <numbers>
#include <vector>

using namespace symbolengine;
using namespace symbolengine::plugin;

// The editor's page, built in from web/.
std::optional<std::string_view> filterEditorFiles(std::string_view path);

class Filter final : public Plugin
{
public:
    static Description describe()
    {
        return {.id = "dev.symbolengine.examples.filter",
                .name = "Filter",
                .vendor = "SymbolEngine",
                .version = "0.1.0",
                .description = "A low pass, high pass or band pass filter.",
                .url = "https://symbolengine.dev",
                .features = {"audio-effect", "filter", "stereo"}};
    }

    Filter()
        : cutoff(addParameter(number(1, "Cutoff", 20.0, 20000.0, 1000.0, "Hz", 0))),
          resonance(addParameter(number(2, "Resonance", 0.5, 10.0, 0.707, "", 2))),
          mode(addParameter(choice(3, "Mode", {"Low Pass", "High Pass", "Band Pass"}, 0)))
    {
    }

    void prepare(double sampleRate, std::uint32_t) override
    {
        rate = sampleRate;
        reset();
    }

    void reset() override { state = {}; }

    void process(Block& block) override
    {
        // The topology-preserving state-variable filter, which stays stable however fast its settings move.
        const double frequency = std::min(cutoff.get(), rate * 0.49);
        const double g = std::tan(std::numbers::pi * frequency / rate);
        const double k = 1.0 / resonance.get();
        const double a1 = 1.0 / (1.0 + g * (g + k));
        const double a2 = g * a1;
        const double a3 = g * a2;
        const auto kind = static_cast<int>(mode.get());

        for (std::size_t channel = 0; channel < block.output().size() && channel < state.size(); ++channel)
        {
            const float* in = block.input()[channel];
            float* out = block.output()[channel];
            auto& [ic1, ic2] = state[channel];
            float loudest = 0.0f;

            for (std::uint32_t i = 0; i < block.frames(); ++i)
            {
                const double v0 = in[i];
                const double v3 = v0 - ic2;
                const double v1 = a1 * ic1 + a2 * v3;
                const double v2 = ic2 + a2 * ic1 + a3 * v3;
                ic1 = 2.0 * v1 - ic1;
                ic2 = 2.0 * v2 - ic2;

                const double y = kind == 0 ? v2 : kind == 1 ? v0 - k * v1 - v2 : v1;
                out[i] = static_cast<float>(y);
                loudest = std::max(loudest, std::abs(out[i]));
            }

            // The loudest sample since the editor last looked, kept without locking.
            auto& peak = peaks[channel];
            float held = peak.load(std::memory_order_relaxed);
            while (loudest > held && !peak.compare_exchange_weak(held, loudest, std::memory_order_relaxed))
            {
            }
        }
    }

    std::optional<Editor> editor() const override
    {
        return Editor{.width = 420,
                      .height = 260,
                      .resizable = true,
                      .minimumWidth = 360,
                      .minimumHeight = 220,
                      .maximumWidth = 1200,
                      .maximumHeight = 800,
                      .files = filterEditorFiles};
    }

    void editorTick(EditorLink& editor) override
    {
        Array levels;
        for (auto& peak : peaks)
            levels.push_back(static_cast<double>(peak.exchange(0.0f, std::memory_order_relaxed)));
        editor.send("levels", std::move(levels));
    }

private:
    ParameterValue cutoff, resonance, mode;
    double rate = 48000.0;
    std::array<std::array<double, 2>, 2> state{};
    std::array<std::atomic<float>, 2> peaks{};
};

SYMBOLENGINE_PLUGINS(Filter)
