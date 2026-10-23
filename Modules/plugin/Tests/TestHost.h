#pragma once

// A host made for the tests, which makes plugins through the same factory and C functions a real host uses, and
// counts what the plugins ask of it.

#include <clap/clap.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
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
            return nullptr;
        };
        host.request_restart = [](const clap_host* h) { ++of(h).restarts; };
        host.request_process = [](const clap_host*) {};
        host.request_callback = [](const clap_host*) {};

        params.rescan = [](const clap_host* h, clap_param_rescan_flags flags) { of(h).rescans.push_back(flags); };
        params.clear = [](const clap_host*, clap_id, clap_param_clear_flags) {};
        params.request_flush = [](const clap_host*) {};
        latency.changed = [](const clap_host* h) { ++of(h).latencyChanges; };
    }

    TestHost(const TestHost&) = delete;
    TestHost& operator=(const TestHost&) = delete;

    static TestHost& of(const clap_host* h) { return *static_cast<TestHost*>(h->host_data); }

    clap_host host{};
    clap_host_params params{};
    clap_host_latency latency{};
    int restarts = 0;
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
