#pragma once

#include <cstdlib>
#include <utility>
#include <vector>

// Choosing among the layouts an Audio Unit offers, apart from Audio Units so the tests can check it plainly.
namespace symbolengine::audioUnit::detail
{

/** One of the layouts a unit says it takes, by the numbers kAudioUnitProperty_SupportedNumChannels gives: a channel count, or
    -1 for any, with -1 on both sides meaning the same on each, and -2 for any at all. */
struct Offer
{
    int inputs = -1;
    int outputs = -1;
};

/** The layout to ask of a unit that wants to be asked one of those: what was wanted when it offers it, and otherwise the
    nearest it does, as one that sets a format it can't take only refuses it as it is initialized. */
inline std::pair<int, int> nearestLayout(const std::vector<Offer>& offers, int inputs, int outputs)
{
    if (offers.empty())
        return {inputs, outputs};

    const auto nearestFor = [&](const Offer& offer) -> std::pair<int, int> {
        if (offer.inputs == -1 && offer.outputs == -1)
        {
            const auto same = outputs > 0 ? outputs : inputs;
            return {same, same};
        }
        return {offer.inputs < 0 ? inputs : offer.inputs, offer.outputs < 0 ? outputs : offer.outputs};
    };

    auto best = nearestFor(offers.front());
    auto bestDistance = std::abs(best.first - inputs) + std::abs(best.second - outputs);
    for (const auto& offer : offers)
    {
        const auto here = nearestFor(offer);
        const auto distance = std::abs(here.first - inputs) + std::abs(here.second - outputs);
        if (distance < bestDistance)
        {
            best = here;
            bestDistance = distance;
        }
    }
    return best;
}

}
