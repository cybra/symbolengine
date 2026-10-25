#include "AudioRecordFormat.h"

#include <array>
#include <utility>

namespace symbolengine::devices::detail
{

namespace
{
constexpr std::array<std::pair<AudioTransport, const char*>, 13> transports{{{AudioTransport::unknown, "unknown"},
                                                                             {AudioTransport::builtIn, "built-in"},
                                                                             {AudioTransport::usb, "USB"},
                                                                             {AudioTransport::firewire, "FireWire"},
                                                                             {AudioTransport::thunderbolt, "Thunderbolt"},
                                                                             {AudioTransport::pci, "PCI"},
                                                                             {AudioTransport::bluetooth, "Bluetooth"},
                                                                             {AudioTransport::hdmi, "HDMI"},
                                                                             {AudioTransport::displayPort, "DisplayPort"},
                                                                             {AudioTransport::airPlay, "AirPlay"},
                                                                             {AudioTransport::network, "network"},
                                                                             {AudioTransport::virtualDevice, "virtual"},
                                                                             {AudioTransport::aggregate, "aggregate"}}};

constexpr std::array<std::pair<AudioSystemChange, const char*>, 6> changes{{{AudioSystemChange::devices, "devices"},
                                                                            {AudioSystemChange::defaults, "defaults"},
                                                                            {AudioSystemChange::serviceRestarted, "service restarted"},
                                                                            {AudioSystemChange::sleeping, "sleeping"},
                                                                            {AudioSystemChange::woke, "woke"},
                                                                            {AudioSystemChange::inputMute, "input mute"}}};

constexpr std::array<std::pair<AudioLoss, const char*>, 7> losses{{{AudioLoss::unplugged, "unplugged"},
                                                                   {AudioLoss::takenExclusively, "taken exclusively"},
                                                                   {AudioLoss::notAnswering, "not answering"},
                                                                   {AudioLoss::serviceRestarted, "service restarted"},
                                                                   {AudioLoss::refused, "refused"},
                                                                   {AudioLoss::clockLost, "clock lost"},
                                                                   {AudioLoss::other, "other"}}};

Array texts(const std::vector<std::string>& from)
{
    Array out;
    for (const auto& text : from)
        out.emplace_back(text);
    return out;
}

std::vector<std::string> textsFrom(const Value& value)
{
    std::vector<std::string> out;
    if (const auto* items = value.ifArray())
        for (const auto& item : *items)
            out.push_back(item.stringOr(""));
    return out;
}
}

Value toValue(const AudioDeviceInfo& device)
{
    Array rates, blocks;
    for (const auto rate : device.sampleRates)
        rates.emplace_back(rate);
    for (const auto block : device.blockSizes)
        blocks.emplace_back(block);
    const char* transport = "unknown";
    for (const auto& [kind, name] : transports)
        if (kind == device.transport)
            transport = name;
    return Object{{"id", device.id},
                  {"name", device.name},
                  {"transport", transport},
                  {"inputs", texts(device.inputNames)},
                  {"outputs", texts(device.outputNames)},
                  {"rates", std::move(rates)},
                  {"blocks", std::move(blocks)},
                  {"rate", device.currentRate},
                  {"block", device.currentBlock},
                  {"handsFree", device.handsFree},
                  {"loopback", device.loopback},
                  {"clockDomain", static_cast<std::int64_t>(device.clockDomain)},
                  {"answering", device.answering},
                  {"problem", device.problem},
                  {"unread", device.unread}};
}

AudioDeviceInfo deviceFrom(const Value& value)
{
    AudioDeviceInfo device;
    device.id = value["id"].stringOr("");
    device.name = value["name"].stringOr("");
    const auto transport = value["transport"].stringOr("unknown");
    for (const auto& [kind, name] : transports)
        if (transport == name)
            device.transport = kind;
    device.inputNames = textsFrom(value["inputs"]);
    device.outputNames = textsFrom(value["outputs"]);
    if (const auto* rates = value["rates"].ifArray())
        for (const auto& rate : *rates)
            device.sampleRates.push_back(rate.numberOr(0.0));
    if (const auto* blocks = value["blocks"].ifArray())
        for (const auto& block : *blocks)
            device.blockSizes.push_back(static_cast<int>(block.intOr(0)));
    device.currentRate = value["rate"].numberOr(0.0);
    device.currentBlock = static_cast<int>(value["block"].intOr(0));
    device.handsFree = value["handsFree"].boolOr(false);
    device.loopback = value["loopback"].boolOr(false);
    device.clockDomain = static_cast<std::uint64_t>(value["clockDomain"].intOr(0));
    device.answering = value["answering"].boolOr(true);
    device.problem = value["problem"].stringOr("");
    device.unread = value["unread"].boolOr(false);
    return device;
}

const char* nameOf(AudioSystemChange change) noexcept
{
    for (const auto& [kind, name] : changes)
        if (kind == change)
            return name;
    return "devices";
}

const char* nameOf(AudioLoss loss) noexcept
{
    for (const auto& [kind, name] : losses)
        if (kind == loss)
            return name;
    return "other";
}

bool changeNamed(const std::string& name, AudioSystemChange& change) noexcept
{
    for (const auto& [kind, text] : changes)
        if (name == text)
        {
            change = kind;
            return true;
        }
    return false;
}

bool lossNamed(const std::string& name, AudioLoss& loss) noexcept
{
    for (const auto& [kind, text] : losses)
        if (name == text)
        {
            loss = kind;
            return true;
        }
    return false;
}

}
