#include "symbolengine/devices/AudioSetup.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace symbolengine
{

namespace
{
constexpr std::size_t remembered = 8;

std::string rateText(double rate)
{
    std::ostringstream text;
    text << rate / 1000.0 << " kHz";
    return text.str();
}

void addSaid(std::string& said, const std::string& sentence)
{
    if (!said.empty())
        said += ' ';
    said += sentence;
}

int channelsOf(const AudioDeviceInfo& device, AudioDirection direction)
{
    return direction == AudioDirection::input ? device.inputs() : device.outputs();
}

const AudioDeviceInfo* byId(const std::vector<AudioDeviceInfo>& devices, const std::string& id)
{
    for (const auto& device : devices)
        if (!id.empty() && device.id == id)
            return &device;
    return nullptr;
}

// What a device may be put in another's place as: one with channels that way that answers, never a hands-free
// microphone or a loopback device unless the user chose it before.
bool canStandIn(const AudioDeviceInfo& device, AudioDirection direction, const AudioSetup::Direction& saved)
{
    if (!device.answering || channelsOf(device, direction) == 0)
        return false;
    if (device.handsFree || device.loopback)
        return std::find(saved.chosenBefore.begin(), saved.chosenBefore.end(), device.id) != saved.chosenBefore.end();
    return true;
}

const AudioDeviceInfo* standIn(const std::vector<AudioDeviceInfo>& devices, const std::string& defaultId,
                               AudioDirection direction, const AudioSetup::Direction& saved)
{
    if (const auto* fallback = byId(devices, defaultId); fallback != nullptr && canStandIn(*fallback, direction, saved))
        return fallback;
    for (const auto& device : devices)
        if (canStandIn(device, direction, saved))
            return &device;
    return nullptr;
}

double chooseRate(double saved, const AudioDeviceInfo& device, std::string& said)
{
    const auto& offered = device.sampleRates;
    if (saved <= 0.0)
    {
        if (device.currentRate > 0.0 || offered.empty())
            return 0.0;
        // The device doesn't say what it runs at: 48 kHz if it offers it, or the highest it offers below.
        double chosen = offered.front();
        for (const auto rate : offered)
            if (rate <= 48000.0 && (chosen > 48000.0 || rate > chosen))
                chosen = rate;
        addSaid(said, device.name + " didn't say its rate, so it runs at " + rateText(chosen) + ".");
        return chosen;
    }
    if (offered.empty())
        return saved;
    for (const auto rate : offered)
        if (std::abs(rate - saved) < 0.5)
            return rate;

    double nearest = offered.front();
    for (const auto rate : offered)
    {
        const auto distance = std::abs(rate - saved), best = std::abs(nearest - saved);
        if (distance < best || (distance == best && rate > nearest))
            nearest = rate;
    }
    addSaid(said, device.name + " doesn't run at " + rateText(saved) + ", so it runs at " + rateText(nearest) + ".");
    return nearest;
}

int chooseBlock(int saved, const AudioDeviceInfo& device, std::string& said)
{
    const auto& offered = device.blockSizes;
    if (saved <= 0 || offered.empty() || std::find(offered.begin(), offered.end(), saved) != offered.end())
        return saved;

    int next = 0, largest = 0;
    for (const auto size : offered)
    {
        largest = std::max(largest, size);
        if (size > saved && (next == 0 || size < next))
            next = size;
    }
    const auto chosen = next != 0 ? next : largest;
    addSaid(said, device.name + " doesn't take blocks of " + std::to_string(saved) + ", so it takes " +
                      std::to_string(chosen) + ".");
    return chosen;
}

AudioResolution::Direction resolveDirection(const AudioSetup& setup, const AudioSetup::Direction& saved,
                                            AudioDirection direction, const std::vector<AudioDeviceInfo>& devices,
                                            const std::string& defaultId)
{
    AudioResolution::Direction found;
    const char* way = direction == AudioDirection::input ? "input" : "output";
    if (saved.choice == AudioSetup::Choice::none)
        return found;

    const AudioDeviceInfo* device = nullptr;
    if (saved.choice == AudioSetup::Choice::systemDefault)
    {
        device = byId(devices, defaultId);
        if (device != nullptr && channelsOf(*device, direction) > 0)
            found.exact = true;
        else
        {
            device = standIn(devices, defaultId, direction, saved);
            if (device != nullptr)
                addSaid(found.said, std::string("There is no default ") + way + ", so " + device->name + " is used.");
        }
    }
    else
    {
        device = byId(devices, saved.id);
        if (device != nullptr)
            found.exact = true;
        else
        {
            // Its identifier gone: a name only one device has, or a question between those that share it.
            std::vector<const AudioDeviceInfo*> sameName;
            for (const auto& candidate : devices)
                if (!saved.name.empty() && candidate.name == saved.name &&
                    (channelsOf(candidate, direction) > 0 || candidate.unread))
                    sameName.push_back(&candidate);
            if (sameName.size() == 1)
            {
                device = sameName.front();
                found.exact = true;
                found.byName = true;
            }
            else if (sameName.size() > 1)
            {
                for (const auto* candidate : sameName)
                    found.ambiguous.push_back(candidate->id);
                addSaid(found.said, "There is more than one " + saved.name + "; which one is the " + way + "?");
                return found;
            }
            else
            {
                device = standIn(devices, defaultId, direction, saved);
                const auto name = saved.name.empty() ? std::string("The ") + way + " chosen" : saved.name;
                addSaid(found.said, device != nullptr ? name + " isn't connected, so " + device->name + " is used."
                                                      : name + " isn't connected.");
            }
        }
    }

    if (device == nullptr)
        return found;

    found.id = device->id;
    found.name = device->name;

    // The channels saved, those the device has; none saved, its first or first two. A driver not yet read is
    // taken at its word: what it has is known once it opens.
    const auto count = channelsOf(*device, direction);
    if (device->unread)
    {
        found.channels = saved.channels;
        if (found.channels.empty())
            found.channels = direction == AudioDirection::output ? std::vector<int>{0, 1} : std::vector<int>{0};
        found.sampleRate = setup.sampleRate;
        found.blockSize = setup.blockSize;
        return found;
    }
    if (saved.channels.empty())
    {
        found.channels.push_back(0);
        if (direction == AudioDirection::output && count >= 2)
            found.channels.push_back(1);
    }
    else
    {
        for (const auto channel : saved.channels)
            if (channel >= 0 && channel < count)
                found.channels.push_back(channel);
            else
                found.channelsMissing = true;
        if (found.channelsMissing)
            addSaid(found.said, device->name + " doesn't have the " + way + " channels chosen.");
    }

    found.sampleRate = chooseRate(setup.sampleRate, *device, found.said);
    found.blockSize = chooseBlock(setup.blockSize, *device, found.said);
    return found;
}

const char* choiceName(AudioSetup::Choice choice)
{
    switch (choice)
    {
        case AudioSetup::Choice::none: return "none";
        case AudioSetup::Choice::device: return "device";
        case AudioSetup::Choice::systemDefault: return "default";
    }
    return "none";
}

Value directionValue(const AudioSetup::Direction& direction)
{
    Array channels, chosenBefore;
    for (const auto channel : direction.channels)
        channels.emplace_back(channel);
    for (const auto& id : direction.chosenBefore)
        chosenBefore.emplace_back(id);
    return Object{{"choice", choiceName(direction.choice)},
                  {"id", direction.id},
                  {"name", direction.name},
                  {"sameNameIndex", direction.sameNameIndex},
                  {"channels", std::move(channels)},
                  {"chosenBefore", std::move(chosenBefore)}};
}

AudioSetup::Direction directionFrom(const Value& value, AudioSetup::Choice fallback)
{
    AudioSetup::Direction direction;
    direction.choice = fallback;
    if (!value.isObject())
        return direction;

    const auto choice = value["choice"].stringOr("");
    if (choice == "none")
        direction.choice = AudioSetup::Choice::none;
    else if (choice == "device")
        direction.choice = AudioSetup::Choice::device;
    else if (choice == "default")
        direction.choice = AudioSetup::Choice::systemDefault;

    direction.id = value["id"].stringOr("");
    direction.name = value["name"].stringOr("");
    direction.sameNameIndex = static_cast<int>(value["sameNameIndex"].intOr(0));
    if (const auto* channels = value["channels"].ifArray())
        for (const auto& channel : *channels)
            if (channel.isInteger())
                direction.channels.push_back(static_cast<int>(channel.asInt()));
    if (const auto* ids = value["chosenBefore"].ifArray())
        for (const auto& id : *ids)
            if (id.isString() && direction.chosenBefore.size() < remembered)
                direction.chosenBefore.push_back(id.asString());
    if (direction.choice == AudioSetup::Choice::device && direction.id.empty() && direction.name.empty())
        direction.choice = fallback;
    return direction;
}
}

void AudioSetup::choose(AudioDirection direction, const AudioDeviceInfo& device, std::vector<int> channels)
{
    auto& chosen = direction == AudioDirection::input ? input : output;
    chosen.choice = Choice::device;
    chosen.id = device.id;
    chosen.name = device.name;
    chosen.sameNameIndex = device.sameNameIndex;
    chosen.channels = std::move(channels);
    std::erase(chosen.chosenBefore, device.id);
    chosen.chosenBefore.insert(chosen.chosenBefore.begin(), device.id);
    if (chosen.chosenBefore.size() > remembered)
        chosen.chosenBefore.resize(remembered);
}

AudioResolution AudioSetup::resolve(const std::vector<AudioDeviceInfo>& devices, const std::string& defaultInput,
                                    const std::string& defaultOutput) const
{
    AudioResolution resolution;
    resolution.input = resolveDirection(*this, input, AudioDirection::input, devices, defaultInput);
    resolution.output = resolveDirection(*this, output, AudioDirection::output, devices, defaultOutput);
    return resolution;
}

Value AudioSetup::toValue() const
{
    return Object{{"system", system},
                  {"input", directionValue(input)},
                  {"output", directionValue(output)},
                  {"sampleRate", sampleRate},
                  {"blockSize", blockSize},
                  {"exclusive", exclusive}};
}

AudioSetup AudioSetup::fromValue(const Value& value)
{
    AudioSetup setup;
    setup.system = value["system"].stringOr("");
    setup.input = directionFrom(value["input"], Choice::none);
    setup.output = directionFrom(value["output"], Choice::systemDefault);
    setup.sampleRate = std::max(0.0, value["sampleRate"].numberOr(0.0));
    setup.blockSize = static_cast<int>(std::max<std::int64_t>(0, value["blockSize"].intOr(0)));
    setup.exclusive = value["exclusive"].boolOr(false);
    return setup;
}

}
