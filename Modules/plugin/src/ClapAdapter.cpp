// Each plugin as a CLAP plugin: the factory hosts make it from, and the functions they call on it, which pass to the
// Plugin class. Every function a host calls catches exceptions, which must never cross into the host's C.

#include <symbolengine/plugin/Plugin.h>

#include <symbolengine/core/Text.h>

#include "Access.h"
#include "Changes.h"
#include "Denormals.h"
#include "Identity.h"
#include "TextValues.h"

#include <clap/clap.h>
#include <clapwrapper/vst3.h>

#if SYMBOLENGINE_EDITORS
    #include "Editor.h"
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
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
struct ComponentId
{
    array_of_16_bytes bytes;
};

// The CLAP descriptors of the plugins this binary offers, made once from their descriptions, and what clap-wrapper
// asks about each one's VST3 build. A class ID written wrongly leaves the binary offering no plugins at all, so the
// mistake shows at once instead of the plugin going out under an ID no session knows.
struct Descriptors
{
    std::vector<std::vector<const char*>> features;
    std::vector<clap_plugin_descriptor> list;
    std::vector<ComponentId> componentIds;
    std::vector<clap_plugin_info_as_vst3> vst3;
};

const Descriptors& descriptors()
{
    static Descriptors made;
    static std::once_flag once;
    std::call_once(once, [] {
        const auto& entries = registered();
        made.features.reserve(entries.size());
        made.list.reserve(entries.size());
        made.componentIds.resize(entries.size());
        made.vst3.resize(entries.size());

        for (std::size_t i = 0; i < entries.size(); ++i)
        {
            const auto& about = entries[i].description;
            auto& features = made.features.emplace_back();
            for (const auto& feature : about.features)
                features.push_back(feature.c_str());
            features.push_back(nullptr);

            made.list.push_back(clap_plugin_descriptor{CLAP_VERSION_INIT, about.id.c_str(), about.name.c_str(),
                                                       about.vendor.c_str(), about.url.c_str(), about.manualUrl.c_str(),
                                                       about.supportUrl.c_str(), about.version.c_str(),
                                                       about.description.c_str(), features.data()});

            made.vst3[i] = {};
            if (!about.vst3ClassId.empty())
            {
                const auto id = detail::vst3ComponentId(about.vst3ClassId);
                if (!id)
                {
                    made = {};
                    return;
                }
                std::memcpy(made.componentIds[i].bytes, id->data(), id->size());
                made.vst3[i].componentId = &made.componentIds[i].bytes;
            }
        }
    });
    return made;
}

class Instance final : public detail::HostLink
{
public:
    Instance(const clap_host* clapHost, const clap_plugin_descriptor* descriptor, const Description& description,
             std::unique_ptr<Plugin> made)
        : host(clapHost), pluginDescription(description), plugin(std::move(made))
    {
        clap.desc = descriptor;
        clap.plugin_data = this;
        clap.init = [](const clap_plugin* p) { return of(p).init(); };
        clap.destroy = [](const clap_plugin* p) { delete &of(p); };
        clap.activate = [](const clap_plugin* p, double rate, std::uint32_t, std::uint32_t maximum) {
            return of(p).activate(rate, maximum);
        };
        clap.deactivate = [](const clap_plugin* p) { of(p).deactivate(); };
        clap.start_processing = [](const clap_plugin* p) {
            of(p).processing = true;
            return true;
        };
        clap.stop_processing = [](const clap_plugin* p) { of(p).processing = false; };
        clap.reset = [](const clap_plugin* p) { of(p).reset(); };
        clap.process = [](const clap_plugin* p, const clap_process* process) { return of(p).process(*process); };
        clap.get_extension = [](const clap_plugin* p, const char* id) { return of(p).extension(id); };
        clap.on_main_thread = [](const clap_plugin*) {};

        detail::Access::attach(*plugin, this);
    }

    ~Instance() override { detail::Access::attach(*plugin, nullptr); }

    clap_plugin clap{};
    const clap_host* host;
    const Description& pluginDescription;
    const clap_host_params* hostParams = nullptr;
    const clap_host_latency* hostLatency = nullptr;
    const clap_host_gui* hostGui = nullptr;
    std::unique_ptr<Plugin> plugin;
    Buses buses;
    bool active = false;
    std::optional<std::uint32_t> reportedLatency;

    // Changes the editor made, for the host to be told of, and whether the host is calling process() to collect them.
    detail::EditorChanges editorChanges;
    std::atomic<bool> processing{false};

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
            hostGui = static_cast<const clap_host_gui*>(host->get_extension(host, CLAP_EXT_GUI));

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

    void editorSizeChanged() override
    {
#if SYMBOLENGINE_EDITORS
        // The plugin set the size its editor opens at; an editor that is open asks the host for it.
        if (editor == nullptr)
            return;
        const auto [w, h] = detail::Access::editorSize(*plugin);
        const auto [held, high] = editor->heldToLimits(w, h);
        if (editor->description().resizable && askHostToResize(held, high))
            editor->resizedTo(held, high);
#endif
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

    void flush(const clap_input_events* in, const clap_output_events* out) noexcept
    {
        const auto count = in != nullptr ? in->size(in) : 0;
        for (std::uint32_t i = 0; i < count; ++i)
            if (const auto* header = in->get(in, i))
                apply(*header);
        tellHost(out);
    }

    // Tells the host of the changes the editor made, as the gestures and values it records automation from.
    void tellHost(const clap_output_events* out) noexcept
    {
        editorChanges.drain([&](const detail::EditorChange& change) {
            if (out == nullptr)
                return;

            if (change.kind == detail::EditorChange::Kind::value)
            {
                clap_event_param_value event{};
                event.header = {sizeof event, 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0};
                event.param_id = change.id;
                if (const auto index = detail::Access::index(*plugin, change.id))
                    event.cookie = reinterpret_cast<void*>(static_cast<std::uintptr_t>(*index) + 1);
                event.note_id = -1;
                event.port_index = -1;
                event.channel = -1;
                event.key = -1;
                event.value = change.value;
                out->try_push(out, &event.header);
            }
            else
            {
                clap_event_param_gesture gesture{};
                const auto type = change.kind == detail::EditorChange::Kind::begin ? CLAP_EVENT_PARAM_GESTURE_BEGIN
                                                                                   : CLAP_EVENT_PARAM_GESTURE_END;
                gesture.header = {sizeof gesture, 0, CLAP_CORE_EVENT_SPACE_ID, static_cast<std::uint16_t>(type), 0};
                gesture.param_id = change.id;
                out->try_push(out, &gesture.header);
            }
        });
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
            tellHost(call.out_events);

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
            copyText(display, size, detail::textFor(*about, value));
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

#if SYMBOLENGINE_EDITORS
    // The editor, in the host's window. A Mac host gives sizes in points; a Windows host gives them in pixels, and
    // says how many pixels a point is.

    #if defined(__APPLE__)
    static constexpr const char* windowApi = CLAP_WINDOW_API_COCOA;
    #else
    static constexpr const char* windowApi = CLAP_WINDOW_API_WIN32;
    #endif

    std::unique_ptr<detail::EditorSession> editor;
    double scale = 1.0;   // pixels to a point, which a Windows host gives

    std::uint32_t pixels(std::uint32_t inPoints) const noexcept
    {
        return static_cast<std::uint32_t>(std::lround(inPoints * scale));
    }

    std::uint32_t points(std::uint32_t inPixels) const noexcept
    {
        return static_cast<std::uint32_t>(std::lround(inPixels / scale));
    }

    void showSize() const
    {
        ui::native::resize(editor->nativeView(), pixels(editor->width()), pixels(editor->height()));
    }

    // Asks the host for a new size in points, and takes it if the host agrees.
    bool askHostToResize(std::uint32_t w, std::uint32_t h)
    {
        if (editor == nullptr || hostGui == nullptr || !hostGui->request_resize(host, pixels(w), pixels(h)))
            return false;
        ui::native::resize(editor->nativeView(), pixels(w), pixels(h));
        return true;
    }

    bool guiSupported(const char* api, bool floating) const noexcept
    {
        try
        {
            return !floating && api != nullptr && std::strcmp(api, windowApi) == 0 && plugin->editor().has_value();
        }
        catch (...)
        {
            return false;
        }
    }

    bool guiCreate(const char* api, bool floating) noexcept
    {
        try
        {
            if (!guiSupported(api, floating))
                return false;

            detail::EditorSession::Host link;
            link.told = [this] {
                // While the plugin isn't processing, the host collects the editor's changes when asked.
                if (!processing && hostParams != nullptr)
                    hostParams->request_flush(host);
            };
            link.resize = [this](std::uint32_t w, std::uint32_t h) { return askHostToResize(w, h); };
            link.pixelsPerPoint = scale;

            editor = std::make_unique<detail::EditorSession>(*plugin, pluginDescription, *plugin->editor(),
                                                             editorChanges, std::move(link));
            if (!editor->available())
            {
                editor.reset();
                return false;
            }
            showSize();
            return true;
        }
        catch (...)
        {
            editor.reset();
            return false;
        }
    }

    bool guiSetScale(double newScale) noexcept
    {
    #if defined(_WIN32)
        if (!(newScale > 0.0))
            return false;
        scale = newScale;
        if (editor != nullptr)
            showSize();
        return true;
    #else
        (void) newScale;
        return false;
    #endif
    }

    bool guiGetSize(std::uint32_t& w, std::uint32_t& h) const noexcept
    {
        if (editor == nullptr)
            return false;
        w = pixels(editor->width());
        h = pixels(editor->height());
        return true;
    }

    bool guiResizable() const noexcept { return editor != nullptr && editor->description().resizable; }

    bool guiResizeHints(clap_gui_resize_hints& hints) const noexcept
    {
        if (editor == nullptr)
            return false;
        hints.can_resize_horizontally = guiResizable();
        hints.can_resize_vertically = guiResizable();
        hints.preserve_aspect_ratio = false;
        hints.aspect_ratio_width = 0;
        hints.aspect_ratio_height = 0;
        return true;
    }

    bool guiAdjustSize(std::uint32_t& w, std::uint32_t& h) const noexcept
    {
        if (editor == nullptr)
            return false;
        const auto [held, high] = editor->heldToLimits(points(w), points(h));
        w = pixels(held);
        h = pixels(high);
        return true;
    }

    bool guiSetSize(std::uint32_t w, std::uint32_t h) noexcept
    {
        if (editor == nullptr)
            return false;
        const auto [held, high] = editor->heldToLimits(points(w), points(h));
        if (pixels(held) != w || pixels(high) != h)
            return false;
        try
        {
            editor->resizedTo(held, high);
        }
        catch (...)
        {
        }
        ui::native::resize(editor->nativeView(), w, h);
        return true;
    }

    bool guiSetParent(const clap_window& window) noexcept
    {
        if (editor == nullptr || window.api == nullptr || std::strcmp(window.api, windowApi) != 0)
            return false;
        if (!ui::native::attach(editor->nativeView(), window.ptr))
            return false;
        showSize();
        return true;
    }

    bool guiShow(bool visible) noexcept
    {
        if (editor == nullptr)
            return false;
        ui::native::show(editor->nativeView(), visible);
        return true;
    }
#endif
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
                const clap_output_events* out) { Instance::of(p).flush(in, out); },
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

#if SYMBOLENGINE_EDITORS
const clap_plugin_gui gui{
    .is_api_supported = [](const clap_plugin* p, const char* api,
                           bool floating) { return Instance::of(p).guiSupported(api, floating); },
    .get_preferred_api =
        [](const clap_plugin*, const char** api, bool* floating) {
            if (api == nullptr || floating == nullptr)
                return false;
            *api = Instance::windowApi;
            *floating = false;
            return true;
        },
    .create = [](const clap_plugin* p, const char* api,
                 bool floating) { return Instance::of(p).guiCreate(api, floating); },
    .destroy = [](const clap_plugin* p) { Instance::of(p).editor.reset(); },
    .set_scale = [](const clap_plugin* p, double scale) { return Instance::of(p).guiSetScale(scale); },
    .get_size = [](const clap_plugin* p, std::uint32_t* w,
                   std::uint32_t* h) { return w != nullptr && h != nullptr && Instance::of(p).guiGetSize(*w, *h); },
    .can_resize = [](const clap_plugin* p) { return Instance::of(p).guiResizable(); },
    .get_resize_hints =
        [](const clap_plugin* p, clap_gui_resize_hints* hints) {
            return hints != nullptr && Instance::of(p).guiResizeHints(*hints);
        },
    .adjust_size =
        [](const clap_plugin* p, std::uint32_t* w, std::uint32_t* h) {
            return w != nullptr && h != nullptr && Instance::of(p).guiAdjustSize(*w, *h);
        },
    .set_size = [](const clap_plugin* p, std::uint32_t w, std::uint32_t h) { return Instance::of(p).guiSetSize(w, h); },
    .set_parent = [](const clap_plugin* p,
                     const clap_window* window) { return window != nullptr && Instance::of(p).guiSetParent(*window); },
    .set_transient = [](const clap_plugin*, const clap_window*) { return false; },
    .suggest_title = [](const clap_plugin*, const char*) {},
    .show = [](const clap_plugin* p) { return Instance::of(p).guiShow(true); },
    .hide = [](const clap_plugin* p) { return Instance::of(p).guiShow(false); },
};
#endif

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
#if SYMBOLENGINE_EDITORS
    if (std::strcmp(id, CLAP_EXT_GUI) == 0)
        return &gui;
#endif
    return nullptr;
}

const clap_plugin_factory_as_vst3 vst3Factory{
    .vendor = nullptr,
    .vendor_url = nullptr,
    .email_contact = nullptr,
    .get_vst3_info = [](const clap_plugin_factory_as_vst3*, std::uint32_t index) -> const clap_plugin_info_as_vst3* {
        const auto& vst3 = descriptors().vst3;
        return index < vst3.size() ? &vst3[index] : nullptr;
    },
    .get_vst3_compatibility = nullptr,
};

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
                    auto* instance =
                        new Instance(host, &descriptors().list[i], entries[i].description, entries[i].make());
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

namespace symbolengine::plugin::detail
{

bool clapInit(const char*) noexcept
{
    return true;
}

void clapDeinit() noexcept {}

std::optional<std::array<std::uint8_t, 16>> vst3ComponentId(std::string_view written)
{
    std::array<std::uint8_t, 16> big{};
    std::size_t digits = 0;
    for (const char c : written)
    {
        if (c == '-' || c == '{' || c == '}' || c == ' ')
            continue;
        int value = -1;
        if (c >= '0' && c <= '9')
            value = c - '0';
        else if (c >= 'a' && c <= 'f')
            value = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
            value = c - 'A' + 10;
        if (value < 0 || digits == 32)
            return std::nullopt;
        big[digits / 2] = static_cast<std::uint8_t>(big[digits / 2] << 4 | value);
        ++digits;
    }
    if (digits != 32)
        return std::nullopt;

    // The first 32 bits in little-endian order, the next two 16 bits each in little-endian order, the rest as written.
    return std::array<std::uint8_t, 16>{big[3], big[2], big[1],  big[0],  big[5],  big[4],  big[7],  big[6],
                                        big[8], big[9], big[10], big[11], big[12], big[13], big[14], big[15]};
}

const void* clapFactory(const char* id) noexcept
{
    if (id == nullptr)
        return nullptr;
    if (std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) == 0)
        return &factory;
    if (std::strcmp(id, CLAP_PLUGIN_FACTORY_INFO_VST3) == 0 || std::strcmp(id, CLAP_PLUGIN_FACTORY_INFO_VST3_V1) == 0)
        return &vst3Factory;
    return nullptr;
}

}
