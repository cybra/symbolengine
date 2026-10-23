// Each plugin as a CLAP plugin: the factory hosts make it from, and the functions they call on it, which pass to the
// Plugin class. Every function a host calls catches exceptions, which must never cross into the host's C.

#include <symbolengine/plugin/Plugin.h>

#include <symbolengine/core/Text.h>

#include "Access.h"
#include "Denormals.h"
#include "TextValues.h"

#include <clap/clap.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace symbolengine::plugin
{

namespace
{

// Copies text into a host's fixed-size field, cut short if it must be, but never in the middle of a UTF-8 character.
void copyText(char* destination, std::size_t size, std::string_view text)
{
    if (size == 0)
        return;
    auto length = std::min(text.size(), size - 1);
    while (length > 0 && length < text.size() && (static_cast<unsigned char>(text[length]) & 0xC0) == 0x80)
        --length;
    std::memcpy(destination, text.data(), length);
    destination[length] = '\0';
}

// The CLAP descriptors of the plugins this binary offers, made once from their descriptions.
struct Descriptors
{
    std::vector<std::vector<const char*>> features;
    std::vector<clap_plugin_descriptor> list;
};

const Descriptors& descriptors()
{
    static Descriptors made;
    static std::once_flag once;
    std::call_once(once, [] {
        const auto& entries = registered();
        made.features.reserve(entries.size());
        made.list.reserve(entries.size());

        for (const auto& entry : entries)
        {
            const auto& about = entry.description;
            auto& features = made.features.emplace_back();
            for (const auto& feature : about.features)
                features.push_back(feature.c_str());
            features.push_back(nullptr);

            made.list.push_back(clap_plugin_descriptor{CLAP_VERSION_INIT, about.id.c_str(), about.name.c_str(),
                                                       about.vendor.c_str(), about.url.c_str(), about.manualUrl.c_str(),
                                                       about.supportUrl.c_str(), about.version.c_str(),
                                                       about.description.c_str(), features.data()});
        }
    });
    return made;
}

class Instance final : public detail::HostLink
{
public:
    Instance(const clap_host* clapHost, const clap_plugin_descriptor* descriptor, std::unique_ptr<Plugin> made)
        : host(clapHost), plugin(std::move(made))
    {
        clap.desc = descriptor;
        clap.plugin_data = this;
        clap.init = [](const clap_plugin* p) { return of(p).init(); };
        clap.destroy = [](const clap_plugin* p) { delete &of(p); };
        clap.activate = [](const clap_plugin* p, double rate, std::uint32_t, std::uint32_t maximum) {
            return of(p).activate(rate, maximum);
        };
        clap.deactivate = [](const clap_plugin* p) { of(p).deactivate(); };
        clap.start_processing = [](const clap_plugin*) { return true; };
        clap.stop_processing = [](const clap_plugin*) {};
        clap.reset = [](const clap_plugin* p) { of(p).reset(); };
        clap.process = [](const clap_plugin* p, const clap_process* process) { return of(p).process(*process); };
        clap.get_extension = [](const clap_plugin* p, const char* id) { return of(p).extension(id); };
        clap.on_main_thread = [](const clap_plugin*) {};

        detail::Access::attach(*plugin, this);
    }

    ~Instance() override { detail::Access::attach(*plugin, nullptr); }

    clap_plugin clap{};
    const clap_host* host;
    const clap_host_params* hostParams = nullptr;
    const clap_host_latency* hostLatency = nullptr;
    std::unique_ptr<Plugin> plugin;
    Buses buses;
    bool active = false;
    std::optional<std::uint32_t> reportedLatency;

    // Each bus's channel pointers, moved along for each stretch of a split block, and the spans a Block hands out.
    std::vector<std::vector<const float*>> inputChannels;
    std::vector<std::vector<float*>> outputChannels;
    std::vector<std::span<const float* const>> inputSpans;
    std::vector<std::span<float* const>> outputSpans;

    static Instance& of(const clap_plugin* p) { return *static_cast<Instance*>(p->plugin_data); }

    bool init() noexcept
    {
        try
        {
            hostParams = static_cast<const clap_host_params*>(host->get_extension(host, CLAP_EXT_PARAMS));
            hostLatency = static_cast<const clap_host_latency*>(host->get_extension(host, CLAP_EXT_LATENCY));

            buses = plugin->buses();
            for (const auto& bus : buses.inputs)
                inputChannels.emplace_back(bus.channels, nullptr);
            for (const auto& bus : buses.outputs)
                outputChannels.emplace_back(bus.channels, nullptr);
            for (const auto& channels : inputChannels)
                inputSpans.emplace_back(channels);
            for (const auto& channels : outputChannels)
                outputSpans.emplace_back(channels);
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    bool activate(double sampleRate, std::uint32_t maximumFrames) noexcept
    {
        try
        {
            plugin->prepare(sampleRate, maximumFrames);

            // The latency may only change while the plugin is being activated.
            const auto latency = plugin->latency();
            if (reportedLatency && *reportedLatency != latency && hostLatency != nullptr)
                hostLatency->changed(host);
            reportedLatency = latency;

            active = true;
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    void deactivate() noexcept
    {
        try
        {
            plugin->release();
        }
        catch (...)
        {
        }
        active = false;
    }

    void reset() noexcept
    {
        try
        {
            plugin->reset();
        }
        catch (...)
        {
        }
    }

    void latencyChanged() override
    {
        // Asked again when the host next activates the plugin, which it does at once when asked to restart.
        if (active)
            host->request_restart(host);
    }

    std::optional<std::size_t> parameterIndex(const clap_event_param_value& change) const noexcept
    {
        if (change.cookie != nullptr)
        {
            const auto index = reinterpret_cast<std::uintptr_t>(change.cookie) - 1;
            if (index < detail::Access::count(*plugin) && plugin->parameters()[index].id == change.param_id)
                return index;
        }
        return detail::Access::index(*plugin, change.param_id);
    }

    // Applies a parameter change, ignoring any aimed at one note or key, which these parameters don't have.
    void apply(const clap_event_header& header) noexcept
    {
        if (header.space_id != CLAP_CORE_EVENT_SPACE_ID || header.type != CLAP_EVENT_PARAM_VALUE)
            return;
        const auto& change = reinterpret_cast<const clap_event_param_value&>(header);
        if (change.note_id != -1 || change.key != -1 || change.channel != -1)
            return;
        if (const auto index = parameterIndex(change))
            detail::Access::set(*plugin, *index, change.value);
    }

    static bool isParameterChange(const clap_event_header& header) noexcept
    {
        return header.space_id == CLAP_CORE_EVENT_SPACE_ID && header.type == CLAP_EVENT_PARAM_VALUE;
    }

    void flush(const clap_input_events* in) noexcept
    {
        const auto count = in != nullptr ? in->size(in) : 0;
        for (std::uint32_t i = 0; i < count; ++i)
            if (const auto* header = in->get(in, i))
                apply(*header);
    }

    bool buffersMatch(const clap_process& call) const noexcept
    {
        if (call.audio_inputs_count != buses.inputs.size() || call.audio_outputs_count != buses.outputs.size())
            return false;
        for (std::size_t i = 0; i < buses.inputs.size(); ++i)
        {
            const auto& buffer = call.audio_inputs[i];
            if (buffer.channel_count != buses.inputs[i].channels ||
                (buffer.channel_count > 0 && buffer.data32 == nullptr))
                return false;
        }
        for (std::size_t i = 0; i < buses.outputs.size(); ++i)
        {
            const auto& buffer = call.audio_outputs[i];
            if (buffer.channel_count != buses.outputs[i].channels ||
                (buffer.channel_count > 0 && buffer.data32 == nullptr))
                return false;
        }
        return true;
    }

    void run(const clap_process& call, std::uint32_t start, std::uint32_t end)
    {
        for (std::size_t bus = 0; bus < inputChannels.size(); ++bus)
            for (std::size_t channel = 0; channel < inputChannels[bus].size(); ++channel)
                inputChannels[bus][channel] = call.audio_inputs[bus].data32[channel] + start;

        for (std::size_t bus = 0; bus < outputChannels.size(); ++bus)
            for (std::size_t channel = 0; channel < outputChannels[bus].size(); ++channel)
                outputChannels[bus][channel] = call.audio_outputs[bus].data32[channel] + start;

        Block block(end - start, inputSpans, outputSpans);
        plugin->process(block);
    }

    clap_process_status process(const clap_process& call) noexcept
    {
        if (!buffersMatch(call))
            return CLAP_PROCESS_ERROR;

        try
        {
            detail::NoDenormals noDenormals;

            // The block is split where each parameter changes, so every stretch is processed with its own values.
            const auto frames = call.frames_count;
            const auto* in = call.in_events;
            const auto count = in != nullptr ? in->size(in) : 0;
            std::uint32_t start = 0;

            for (std::uint32_t i = 0; i < count; ++i)
            {
                const auto* header = in->get(in, i);
                if (header == nullptr || !isParameterChange(*header))
                    continue;
                const auto at = std::min(header->time, frames);
                if (at > start)
                {
                    run(call, start, at);
                    start = at;
                }
                apply(*header);
            }

            if (start < frames)
                run(call, start, frames);

            for (std::size_t bus = 0; bus < buses.outputs.size(); ++bus)
                call.audio_outputs[bus].constant_mask = 0;

            return CLAP_PROCESS_CONTINUE;
        }
        catch (...)
        {
            return CLAP_PROCESS_ERROR;
        }
    }

    // Parameters

    const Parameter* parameter(clap_id id) const noexcept { return plugin->findParameter(id); }

    bool parameterInfo(std::uint32_t index, clap_param_info& info) const noexcept
    {
        if (index >= detail::Access::count(*plugin))
            return false;

        const auto& about = plugin->parameters()[index];
        info = {};
        info.id = about.id;
        if (about.automatable)
            info.flags |= CLAP_PARAM_IS_AUTOMATABLE;
        if (about.stepped)
            info.flags |= CLAP_PARAM_IS_STEPPED;
        if (about.hidden)
            info.flags |= CLAP_PARAM_IS_HIDDEN;
        if (about.bypass)
            info.flags |= CLAP_PARAM_IS_BYPASS;
        info.cookie = reinterpret_cast<void*>(static_cast<std::uintptr_t>(index) + 1);
        copyText(info.name, sizeof info.name, about.name);
        copyText(info.module, sizeof info.module, about.group);
        info.min_value = about.minimum;
        info.max_value = about.maximum;
        info.default_value = about.defaultValue;
        return true;
    }

    bool parameterValue(clap_id id, double& value) const noexcept
    {
        const auto index = detail::Access::index(*plugin, id);
        if (!index)
            return false;
        value = detail::Access::value(*plugin, *index);
        return true;
    }

    bool valueToText(clap_id id, double value, char* display, std::uint32_t size) const noexcept
    {
        try
        {
            const auto* about = parameter(id);
            if (about == nullptr)
                return false;
            copyText(display, size, about->toText ? about->toText(value) : text::formatFixed(value, 2));
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    bool textToValue(clap_id id, const char* display, double& value) const noexcept
    {
        try
        {
            const auto* about = parameter(id);
            if (about == nullptr || display == nullptr)
                return false;
            const auto read = about->fromText ? about->fromText(display) : detail::leadingNumber(display);
            if (!read || std::isnan(*read))
                return false;
            value = std::clamp(*read, about->minimum, about->maximum);
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    // State

    bool saveState(const clap_ostream& stream) const noexcept
    {
        try
        {
            const auto bytes = plugin->saveState();
            std::size_t written = 0;
            while (written < bytes.size())
            {
                const auto wrote = stream.write(&stream, bytes.data() + written, bytes.size() - written);
                if (wrote <= 0)
                    return false;
                written += static_cast<std::size_t>(wrote);
            }
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    bool loadState(const clap_istream& stream) noexcept
    {
        try
        {
            std::string bytes;
            char buffer[65536];
            for (;;)
            {
                const auto got = stream.read(&stream, buffer, sizeof buffer);
                if (got < 0)
                    return false;
                if (got == 0)
                    break;
                bytes.append(buffer, static_cast<std::size_t>(got));
            }

            if (!plugin->loadState(bytes))
                return false;

            if (hostParams != nullptr)
                hostParams->rescan(host, CLAP_PARAM_RESCAN_VALUES);
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    // Audio ports

    std::uint32_t portCount(bool input) const noexcept
    {
        return static_cast<std::uint32_t>(input ? buses.inputs.size() : buses.outputs.size());
    }

    bool portInfo(std::uint32_t index, bool input, clap_audio_port_info& info) const noexcept
    {
        const auto& list = input ? buses.inputs : buses.outputs;
        const auto& other = input ? buses.outputs : buses.inputs;
        if (index >= list.size())
            return false;

        const auto& bus = list[index];
        info = {};
        info.id = index;
        copyText(info.name, sizeof info.name, bus.name);
        if (index == 0)
            info.flags |= CLAP_AUDIO_PORT_IS_MAIN;
        info.channel_count = bus.channels;
        info.port_type = bus.channels == 1 ? CLAP_PORT_MONO : bus.channels == 2 ? CLAP_PORT_STEREO : nullptr;
        info.in_place_pair = index < other.size() && other[index].channels == bus.channels ? index : CLAP_INVALID_ID;
        return true;
    }

    std::uint32_t latency() const noexcept
    {
        try
        {
            return plugin->latency();
        }
        catch (...)
        {
            return 0;
        }
    }

    std::uint32_t tail() const noexcept
    {
        try
        {
            return plugin->tail();
        }
        catch (...)
        {
            return 0;
        }
    }

    const void* extension(const char* id) const noexcept;
};

// The extensions each plugin offers.

const clap_plugin_params params{
    .count =
        [](const clap_plugin* p) { return static_cast<std::uint32_t>(detail::Access::count(*Instance::of(p).plugin)); },
    .get_info = [](const clap_plugin* p, std::uint32_t index,
                   clap_param_info* info) { return info != nullptr && Instance::of(p).parameterInfo(index, *info); },
    .get_value = [](const clap_plugin* p, clap_id id,
                    double* value) { return value != nullptr && Instance::of(p).parameterValue(id, *value); },
    .value_to_text =
        [](const clap_plugin* p, clap_id id, double value, char* display, std::uint32_t size) {
            return display != nullptr && Instance::of(p).valueToText(id, value, display, size);
        },
    .text_to_value = [](const clap_plugin* p, clap_id id, const char* display,
                        double* value) { return value != nullptr && Instance::of(p).textToValue(id, display, *value); },
    .flush = [](const clap_plugin* p, const clap_input_events* in,
                const clap_output_events*) { Instance::of(p).flush(in); },
};

const clap_plugin_state state{
    .save = [](const clap_plugin* p,
               const clap_ostream* stream) { return stream != nullptr && Instance::of(p).saveState(*stream); },
    .load = [](const clap_plugin* p,
               const clap_istream* stream) { return stream != nullptr && Instance::of(p).loadState(*stream); },
};

const clap_plugin_audio_ports audioPorts{
    .count = [](const clap_plugin* p, bool input) { return Instance::of(p).portCount(input); },
    .get = [](const clap_plugin* p, std::uint32_t index, bool input,
              clap_audio_port_info* info) { return info != nullptr && Instance::of(p).portInfo(index, input, *info); },
};

const clap_plugin_latency latencies{
    .get = [](const clap_plugin* p) { return Instance::of(p).latency(); },
};

const clap_plugin_tail tails{
    .get = [](const clap_plugin* p) { return Instance::of(p).tail(); },
};

const void* Instance::extension(const char* id) const noexcept
{
    if (id == nullptr)
        return nullptr;
    if (std::strcmp(id, CLAP_EXT_PARAMS) == 0)
        return &params;
    if (std::strcmp(id, CLAP_EXT_STATE) == 0)
        return &state;
    if (std::strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0)
        return &audioPorts;
    if (std::strcmp(id, CLAP_EXT_LATENCY) == 0)
        return &latencies;
    if (std::strcmp(id, CLAP_EXT_TAIL) == 0)
        return &tails;
    return nullptr;
}

const clap_plugin_factory factory{
    .get_plugin_count =
        [](const clap_plugin_factory*) { return static_cast<std::uint32_t>(descriptors().list.size()); },
    .get_plugin_descriptor = [](const clap_plugin_factory*, std::uint32_t index) -> const clap_plugin_descriptor* {
        const auto& list = descriptors().list;
        return index < list.size() ? &list[index] : nullptr;
    },
    .create_plugin = [](const clap_plugin_factory*, const clap_host* host, const char* id) -> const clap_plugin* {
        if (host == nullptr || id == nullptr || !clap_version_is_compatible(host->clap_version))
            return nullptr;

        try
        {
            const auto& entries = registered();
            for (std::size_t i = 0; i < entries.size(); ++i)
            {
                if (entries[i].description.id == id)
                {
                    auto* instance = new Instance(host, &descriptors().list[i], entries[i].make());
                    return &instance->clap;
                }
            }
        }
        catch (...)
        {
        }
        return nullptr;
    },
};

}

}

extern "C"
{

    bool symbolengine_clap_init(const char*)
    {
        return true;
    }

    void symbolengine_clap_deinit(void) {}

    const void* symbolengine_clap_get_factory(const char* id)
    {
        if (id != nullptr && std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) == 0)
            return &symbolengine::plugin::factory;
        return nullptr;
    }
}
