// Loads a CLAP plugin binary as a host does, then makes, runs and destroys each plugin it offers. A binary that can't
// load, such as one missing a symbol, fails here on every system, sanitizer builds included.

#include <clap/clap.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#else
    #include <dlfcn.h>
#endif

namespace
{
int fail(const char* what)
{
    std::fprintf(stderr, "%s\n", what);
    return 1;
}

const clap_plugin_entry* load(const char* path)
{
#if defined(_WIN32)
    const HMODULE library = LoadLibraryA(path);
    if (library == nullptr)
        return nullptr;
    return reinterpret_cast<const clap_plugin_entry*>(reinterpret_cast<void*>(GetProcAddress(library, "clap_entry")));
#else
    void* library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (library == nullptr)
    {
        std::fprintf(stderr, "%s\n", dlerror());
        return nullptr;
    }
    return static_cast<const clap_plugin_entry*>(dlsym(library, "clap_entry"));
#endif
}

struct Buffers
{
    std::vector<std::vector<float>> samples;
    std::vector<float*> channels;
    clap_audio_buffer buffer{};
};

// Each of a plugin's ports in one direction, with a buffer of the size it says.
std::vector<Buffers> buffersFor(const clap_plugin* plugin, bool input, std::uint32_t frames)
{
    std::vector<Buffers> ports;
    const auto* audioPorts =
        static_cast<const clap_plugin_audio_ports*>(plugin->get_extension(plugin, CLAP_EXT_AUDIO_PORTS));
    const auto count = audioPorts != nullptr ? audioPorts->count(plugin, input) : 0;
    ports.resize(count);

    for (std::uint32_t i = 0; i < count; ++i)
    {
        clap_audio_port_info info{};
        audioPorts->get(plugin, i, input, &info);
        auto& port = ports[i];
        port.samples.assign(info.channel_count, std::vector<float>(frames));
        for (auto& channel : port.samples)
        {
            for (std::uint32_t n = 0; n < frames; ++n)
                channel[n] = input ? 0.5f * static_cast<float>(std::sin(0.05 * n)) : 0.0f;
            port.channels.push_back(channel.data());
        }
        port.buffer.data32 = port.channels.data();
        port.buffer.channel_count = info.channel_count;
    }
    return ports;
}
}

int main(int argc, char** argv)
{
    if (argc != 2)
        return fail("usage: clap_loader <plugin binary>");

    const auto* entry = load(argv[1]);
    if (entry == nullptr)
        return fail("the binary has no clap_entry, or couldn't be loaded");
    if (!clap_version_is_compatible(entry->clap_version) || !entry->init(argv[1]))
        return fail("the entry point wouldn't start");

    const auto* factory = static_cast<const clap_plugin_factory*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    if (factory == nullptr || factory->get_plugin_count(factory) == 0)
        return fail("the binary offers no plugins");

    clap_host host{};
    host.clap_version = CLAP_VERSION;
    host.name = "clap_loader";
    host.vendor = "SymbolEngine";
    host.url = "";
    host.version = "1";
    host.get_extension = [](const clap_host*, const char*) -> const void* { return nullptr; };
    host.request_restart = [](const clap_host*) {};
    host.request_process = [](const clap_host*) {};
    host.request_callback = [](const clap_host*) {};

    const clap_input_events noEvents{
        nullptr, [](const clap_input_events*) { return 0u; },
        [](const clap_input_events*, std::uint32_t) -> const clap_event_header* { return nullptr; }};
    const clap_output_events ignored{nullptr, [](const clap_output_events*, const clap_event_header*) { return true; }};

    constexpr std::uint32_t frames = 256;

    for (std::uint32_t i = 0; i < factory->get_plugin_count(factory); ++i)
    {
        const auto* descriptor = factory->get_plugin_descriptor(factory, i);
        const auto* plugin = factory->create_plugin(factory, &host, descriptor->id);
        if (plugin == nullptr || !plugin->init(plugin))
            return fail("a plugin couldn't be made");
        if (!plugin->activate(plugin, 48000.0, 1, frames) || !plugin->start_processing(plugin))
            return fail("a plugin couldn't start");

        auto inputs = buffersFor(plugin, true, frames);
        auto outputs = buffersFor(plugin, false, frames);
        std::vector<clap_audio_buffer> in, out;
        for (auto& port : inputs)
            in.push_back(port.buffer);
        for (auto& port : outputs)
            out.push_back(port.buffer);

        clap_process call{};
        call.steady_time = 0;
        call.frames_count = frames;
        call.audio_inputs = in.data();
        call.audio_outputs = out.data();
        call.audio_inputs_count = static_cast<std::uint32_t>(in.size());
        call.audio_outputs_count = static_cast<std::uint32_t>(out.size());
        call.in_events = &noEvents;
        call.out_events = &ignored;

        for (int block = 0; block < 4; ++block, call.steady_time += frames)
            if (plugin->process(plugin, &call) == CLAP_PROCESS_ERROR)
                return fail("a plugin failed to process");

        for (const auto& port : outputs)
            for (const auto& channel : port.samples)
                for (const float sample : channel)
                    if (!std::isfinite(sample))
                        return fail("a plugin's output isn't finite");

        plugin->stop_processing(plugin);
        plugin->deactivate(plugin);
        plugin->destroy(plugin);
        std::printf("%s: made, processed and destroyed\n", descriptor->id);
    }

    entry->deinit();
    return 0;
}
