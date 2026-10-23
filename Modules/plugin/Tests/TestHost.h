#pragma once

// A host made for the tests, which makes plugins through the same factory and C functions a real host uses, and
// counts what the plugins ask of it.

#include <clap/clap.h>
#include <clapwrapper/wide_speakers.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

extern "C" const void* symbolengine_clap_get_factory(const char* id);

inline const clap_plugin_factory& factory()
{
    return *static_cast<const clap_plugin_factory*>(symbolengine_clap_get_factory(CLAP_PLUGIN_FACTORY_ID));
}

struct TestHost
{
    TestHost()
    {
        host.clap_version = CLAP_VERSION;
        host.host_data = this;
        host.name = "Test host";
        host.vendor = "SymbolEngine";
        host.url = "";
        host.version = "1";
        host.get_extension = [](const clap_host* h, const char* id) -> const void* {
            if (std::strcmp(id, CLAP_EXT_PARAMS) == 0)
                return &of(h).params;
            if (std::strcmp(id, CLAP_EXT_LATENCY) == 0)
                return &of(h).latency;
            if (std::strcmp(id, CLAP_EXT_GUI) == 0)
                return &of(h).gui;
            if (std::strcmp(id, CLAP_EXT_STATE) == 0)
                return &of(h).state;
            if (std::strcmp(id, CLAP_WRAPPER_WIDE_SPEAKERS) == 0 && of(h).offerWideSpeakers)
                return &of(h).wideSpeakers;
            return nullptr;
        };
        host.request_restart = [](const clap_host* h) { ++of(h).restarts; };
        host.request_process = [](const clap_host*) {};
        host.request_callback = [](const clap_host*) {};

        params.rescan = [](const clap_host* h, clap_param_rescan_flags flags) { of(h).rescans.push_back(flags); };
        params.clear = [](const clap_host*, clap_id, clap_param_clear_flags) {};
        params.request_flush = [](const clap_host* h) { ++of(h).flushRequests; };
        latency.changed = [](const clap_host* h) { ++of(h).latencyChanges; };
        state.mark_dirty = [](const clap_host* h) { ++of(h).dirtied; };
        gui.resize_hints_changed = [](const clap_host*) {};
        gui.request_resize = [](const clap_host* h, std::uint32_t w, std::uint32_t height) {
            of(h).resizeRequests.emplace_back(w, height);
            return of(h).agreeToResize;
        };
        gui.request_show = [](const clap_host*) { return true; };
        gui.request_hide = [](const clap_host*) { return true; };
        gui.closed = [](const clap_host*, bool) {};
    }

    TestHost(const TestHost&) = delete;
    TestHost& operator=(const TestHost&) = delete;

    static TestHost& of(const clap_host* h) { return *static_cast<TestHost*>(h->host_data); }

    clap_host host{};
    clap_host_params params{};
    clap_host_latency latency{};
    clap_host_gui gui{};
    clap_host_state state{};
    int dirtied = 0;
    clap_wrapper_wide_speakers wideSpeakers{};
    bool offerWideSpeakers = false;   // as clap-wrapper's VST3 and AU wrappers do
    int restarts = 0;
    int flushRequests = 0;
    bool agreeToResize = true;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> resizeRequests;
    int latencyChanges = 0;
    std::vector<clap_param_rescan_flags> rescans;
};

// A plugin made through the factory and initialized, destroyed when the test is done with it.
struct Made
{
    Made(TestHost& host, const char* id) : plugin(factory().create_plugin(&factory(), &host.host, id))
    {
        if (plugin != nullptr && !plugin->init(plugin))
        {
            plugin->destroy(plugin);
            plugin = nullptr;
        }
    }

    ~Made()
    {
        if (plugin != nullptr)
            plugin->destroy(plugin);
    }

    Made(const Made&) = delete;
    Made& operator=(const Made&) = delete;

    template <typename Extension>
    const Extension& extension(const char* id) const
    {
        return *static_cast<const Extension*>(plugin->get_extension(plugin, id));
    }

    const clap_plugin* plugin;
};

// Parameter changes as a host sends them.
struct Changes
{
    Changes() = default;
    Changes(const Changes&) = delete;
    Changes& operator=(const Changes&) = delete;

    Changes& add(std::uint32_t time, clap_id id, double value, void* cookie = nullptr, std::int32_t noteId = -1)
    {
        clap_event_param_value change{};
        change.header = {sizeof(clap_event_param_value), time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0};
        change.param_id = id;
        change.cookie = cookie;
        change.note_id = noteId;
        change.port_index = -1;
        change.channel = -1;
        change.key = -1;
        change.value = value;
        changes.push_back(change);
        return *this;
    }

    std::vector<clap_event_param_value> changes;
    clap_input_events list{this,
                           [](const clap_input_events* l) {
                               return static_cast<std::uint32_t>(static_cast<const Changes*>(l->ctx)->changes.size());
                           },
                           [](const clap_input_events* l, std::uint32_t i) -> const clap_event_header* {
                               return &static_cast<const Changes*>(l->ctx)->changes[i].header;
                           }};
};

// Stereo audio in and out, and a process call over it.
struct Stereo
{
    explicit Stereo(std::uint32_t frames)
        : frameCount(frames), in{std::vector<float>(frames), std::vector<float>(frames)},
          out{std::vector<float>(frames, 99.0f), std::vector<float>(frames, 99.0f)}
    {
        for (std::uint32_t i = 0; i < frames; ++i)
        {
            in[0][i] = static_cast<float>(i + 1) / static_cast<float>(frames);
            in[1][i] = -in[0][i];
        }
        inPointers[0] = in[0].data();
        inPointers[1] = in[1].data();
        outPointers[0] = out[0].data();
        outPointers[1] = out[1].data();
        input = {inPointers, nullptr, 2, 0, 0};
        output = {outPointers, nullptr, 2, 0, 0};
    }

    Stereo(const Stereo&) = delete;
    Stereo& operator=(const Stereo&) = delete;

    clap_process_status run(const clap_plugin* plugin, const Changes& changes)
    {
        clap_output_events outEvents{nullptr, [](const clap_output_events*, const clap_event_header*) { return true; }};
        clap_process call{};
        call.steady_time = -1;
        call.frames_count = frameCount;
        call.audio_inputs = &input;
        call.audio_outputs = &output;
        call.audio_inputs_count = 1;
        call.audio_outputs_count = 1;
        call.in_events = &changes.list;
        call.out_events = &outEvents;
        return plugin->process(plugin, &call);
    }

    std::uint32_t frameCount;
    std::vector<float> in[2];
    std::vector<float> out[2];
    float* inPointers[2] = {};
    float* outPointers[2] = {};
    clap_audio_buffer input{};
    clap_audio_buffer output{};
};

// Audio with any number of channels in and out, and a process call over it.
struct Audio
{
    Audio(std::uint32_t frames, std::uint32_t inputs, std::uint32_t outputs)
        : frameCount(frames), in(inputs, std::vector<float>(frames)), out(outputs, std::vector<float>(frames, 99.0f))
    {
        for (std::uint32_t i = 0; i < frames; ++i)
            for (auto& channel : in)
                channel[i] = static_cast<float>(i + 1) / static_cast<float>(frames);
        for (auto& channel : in)
            inPointers.push_back(channel.data());
        for (auto& channel : out)
            outPointers.push_back(channel.data());
        input = {inPointers.data(), nullptr, inputs, 0, 0};
        output = {outPointers.data(), nullptr, outputs, 0, 0};
    }

    Audio(const Audio&) = delete;
    Audio& operator=(const Audio&) = delete;

    clap_process_status run(const clap_plugin* plugin)
    {
        Changes none;
        clap_output_events outEvents{nullptr, [](const clap_output_events*, const clap_event_header*) { return true; }};
        clap_process call{};
        call.steady_time = -1;
        call.frames_count = frameCount;
        call.audio_inputs = &input;
        call.audio_outputs = &output;
        call.audio_inputs_count = 1;
        call.audio_outputs_count = 1;
        call.in_events = &none.list;
        call.out_events = &outEvents;
        return plugin->process(plugin, &call);
    }

    std::uint32_t frameCount;
    std::vector<std::vector<float>> in;
    std::vector<std::vector<float>> out;
    std::vector<float*> inPointers;
    std::vector<float*> outPointers;
    clap_audio_buffer input{};
    clap_audio_buffer output{};
};

// A port configuration as a host asks for one: by type and channel map, or by channel count alone.
struct Request
{
    static Request typed(bool input, std::uint32_t channels, const char* type)
    {
        return {{input, 0, channels, type, nullptr}, {}};
    }

    static Request mapped(bool input, std::vector<std::uint8_t> map)
    {
        Request request{{input, 0, static_cast<std::uint32_t>(map.size()), CLAP_PORT_SURROUND, nullptr},
                        std::move(map)};
        return request;
    }

    static Request counted(bool input, std::uint32_t channels) { return {{input, 0, channels, nullptr, nullptr}, {}}; }

    clap_audio_port_configuration_request request;
    std::vector<std::uint8_t> map;
};

// Whether a plugin can take, or takes, the requests together.
inline bool configure(const clap_plugin* plugin, std::vector<Request> requests, bool apply)
{
    std::vector<clap_audio_port_configuration_request> list;
    for (auto& request : requests)
    {
        if (!request.map.empty())
            request.request.port_details = request.map.data();
        list.push_back(request.request);
    }
    const auto& ports = *static_cast<const clap_plugin_configurable_audio_ports*>(
        plugin->get_extension(plugin, CLAP_EXT_CONFIGURABLE_AUDIO_PORTS));
    return apply ? ports.apply_configuration(plugin, list.data(), static_cast<std::uint32_t>(list.size()))
                 : ports.can_apply_configuration(plugin, list.data(), static_cast<std::uint32_t>(list.size()));
}

// The speaker of each channel of a port, as the plugin tells a host.
inline std::vector<std::uint8_t> channelMap(const clap_plugin* plugin, bool input)
{
    const auto& surround = *static_cast<const clap_plugin_surround*>(plugin->get_extension(plugin, CLAP_EXT_SURROUND));
    std::vector<std::uint8_t> map(32);
    map.resize(surround.get_channel_map(plugin, input, 0, map.data(), static_cast<std::uint32_t>(map.size())));
    return map;
}

// A stream a plugin saves its state into or loads it from.
struct Bytes
{
    Bytes() = default;
    Bytes(const Bytes&) = delete;
    Bytes& operator=(const Bytes&) = delete;

    std::string bytes;
    std::size_t readAt = 0;

    clap_ostream writer{this, [](const clap_ostream* s, const void* data, std::uint64_t size) -> std::int64_t {
                            // Takes at most 7 bytes at a time, as a host may, so the plugin has to keep writing.
                            const auto taken = size < 7 ? size : 7;
                            static_cast<Bytes*>(s->ctx)->bytes.append(static_cast<const char*>(data), taken);
                            return static_cast<std::int64_t>(taken);
                        }};

    clap_istream reader{this, [](const clap_istream* s, void* data, std::uint64_t size) -> std::int64_t {
                            auto& self = *static_cast<Bytes*>(s->ctx);
                            const auto left = self.bytes.size() - self.readAt;
                            const auto given = std::min<std::uint64_t>({size, left, 5});
                            std::memcpy(data, self.bytes.data() + self.readAt, given);
                            self.readAt += given;
                            return static_cast<std::int64_t>(given);
                        }};
};
