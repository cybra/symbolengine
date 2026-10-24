// A stereo pass-through CLAP plugin with PARAMS parameters (0 or 1), for reproducing clap-validator's reports beside
// this file. Its one parameter, when it has one, is a stepped, automatable switch from 0 to 1 that starts at START, 0
// unless built otherwise, as a bypass or an on/off switch does.
//
// On a Mac, with CLAP's headers in clap/include, build it into a bundle:
//
//   mkdir -p "Lone Switch.clap/Contents/MacOS"
//   cc -std=c11 -shared -fvisibility=hidden -DPARAMS=1 -Iclap/include clap-validator-test-plugin.c \
//       -o "Lone Switch.clap/Contents/MacOS/Lone Switch"
//
// and give it a Contents/Info.plist with CFBundleExecutable "Lone Switch" and CFBundlePackageType BNDL, then sign it
// ad hoc with codesign --force --sign - "Lone Switch.clap". -DPARAMS=0 builds the one with no parameters, and
// -DSTART=1.0 the switch that starts at 1.
#include <clap/clap.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef PARAMS
#define PARAMS 1
#endif
#ifndef START
#define START 0.0
#endif

#if PARAMS
#define PLUGIN_ID "example.lone-switch"
#define PLUGIN_NAME "Lone Switch"
#else
#define PLUGIN_ID "example.no-parameters"
#define PLUGIN_NAME "No Parameters"
#endif

typedef struct
{
    clap_plugin_t plugin;
    const clap_host_t* host;
    double value;
} Minimal;

static const char* features[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, NULL};
static const clap_plugin_descriptor_t descriptor = {
    CLAP_VERSION_INIT, PLUGIN_ID, PLUGIN_NAME, "Example", "", "", "", "1.0.0", "", features};

static Minimal* self(const clap_plugin_t* plugin) { return (Minimal*)plugin->plugin_data; }

static void apply(Minimal* m, const clap_input_events_t* in)
{
    for (uint32_t i = 0, n = in->size(in); i < n; ++i)
    {
        const clap_event_header_t* header = in->get(in, i);
        if (header->space_id == CLAP_CORE_EVENT_SPACE_ID && header->type == CLAP_EVENT_PARAM_VALUE)
        {
            const clap_event_param_value_t* event = (const clap_event_param_value_t*)header;
            if (event->param_id == 1)
                m->value = event->value < 0.5 ? 0.0 : 1.0;
        }
    }
}

// audio-ports: one stereo port in and one out.
static uint32_t ports_count(const clap_plugin_t* plugin, bool is_input) { return 1; }
static bool ports_get(const clap_plugin_t* plugin, uint32_t index, bool is_input, clap_audio_port_info_t* info)
{
    if (index != 0)
        return false;
    info->id = 0;
    snprintf(info->name, sizeof(info->name), "%s", is_input ? "In" : "Out");
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = 0;
    return true;
}
static const clap_plugin_audio_ports_t ports = {ports_count, ports_get};

// params: PARAMS of them.
static uint32_t params_count(const clap_plugin_t* plugin) { return PARAMS; }
static bool params_info(const clap_plugin_t* plugin, uint32_t index, clap_param_info_t* info)
{
    if (index >= PARAMS)
        return false;
    memset(info, 0, sizeof(*info));
    info->id = 1;
    info->flags = CLAP_PARAM_IS_STEPPED | CLAP_PARAM_IS_AUTOMATABLE;
    snprintf(info->name, sizeof(info->name), "Switch");
    info->min_value = 0.0;
    info->max_value = 1.0;
    info->default_value = START;
    return true;
}
static bool params_value(const clap_plugin_t* plugin, clap_id id, double* value)
{
    if (id != 1 || !PARAMS)
        return false;
    *value = self(plugin)->value;
    return true;
}
static bool params_to_text(const clap_plugin_t* plugin, clap_id id, double value, char* text, uint32_t size)
{
    if (id != 1 || !PARAMS)
        return false;
    snprintf(text, size, "%s", value < 0.5 ? "Off" : "On");
    return true;
}
static bool params_from_text(const clap_plugin_t* plugin, clap_id id, const char* text, double* value)
{
    if (id != 1 || !PARAMS)
        return false;
    if (strcmp(text, "Off") == 0)
        *value = 0.0;
    else if (strcmp(text, "On") == 0)
        *value = 1.0;
    else
        return false;
    return true;
}
static void params_flush(const clap_plugin_t* plugin, const clap_input_events_t* in, const clap_output_events_t* out)
{
    apply(self(plugin), in);
}
static const clap_plugin_params_t params = {params_count, params_info, params_value, params_to_text, params_from_text,
                                            params_flush};

// state: the switch's value.
static bool state_save(const clap_plugin_t* plugin, const clap_ostream_t* stream)
{
    const double value = self(plugin)->value;
    return stream->write(stream, &value, sizeof(value)) == sizeof(value);
}
static bool state_load(const clap_plugin_t* plugin, const clap_istream_t* stream)
{
    double value = 0.0;
    if (stream->read(stream, &value, sizeof(value)) != sizeof(value) || (value != 0.0 && value != 1.0))
        return false;
    Minimal* m = self(plugin);
    if (value != m->value)
    {
        m->value = value;
        const clap_host_params_t* host_params = m->host->get_extension(m->host, CLAP_EXT_PARAMS);
        if (host_params)
            host_params->rescan(m->host, CLAP_PARAM_RESCAN_VALUES);
    }
    return true;
}
static const clap_plugin_state_t state = {state_save, state_load};

static bool plugin_init(const clap_plugin_t* plugin) { return true; }
static void plugin_destroy(const clap_plugin_t* plugin) { free(self(plugin)); }
static bool plugin_activate(const clap_plugin_t* plugin, double rate, uint32_t min_frames, uint32_t max_frames) { return true; }
static void plugin_deactivate(const clap_plugin_t* plugin) {}
static bool plugin_start(const clap_plugin_t* plugin) { return true; }
static void plugin_stop(const clap_plugin_t* plugin) {}
static void plugin_reset(const clap_plugin_t* plugin) {}
static clap_process_status plugin_process(const clap_plugin_t* plugin, const clap_process_t* process)
{
    apply(self(plugin), process->in_events);
    for (uint32_t c = 0; c < 2; ++c)
        if (process->audio_outputs[0].data32[c] != process->audio_inputs[0].data32[c])
            memcpy(process->audio_outputs[0].data32[c], process->audio_inputs[0].data32[c],
                   sizeof(float) * process->frames_count);
    return CLAP_PROCESS_CONTINUE;
}
static const void* plugin_extension(const clap_plugin_t* plugin, const char* id)
{
    if (strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0)
        return &ports;
    if (strcmp(id, CLAP_EXT_PARAMS) == 0)
        return &params;
    if (strcmp(id, CLAP_EXT_STATE) == 0)
        return &state;
    return NULL;
}
static void plugin_main_thread(const clap_plugin_t* plugin) {}

static uint32_t factory_count(const clap_plugin_factory_t* factory) { return 1; }
static const clap_plugin_descriptor_t* factory_descriptor(const clap_plugin_factory_t* factory, uint32_t index)
{
    return index == 0 ? &descriptor : NULL;
}
static const clap_plugin_t* factory_create(const clap_plugin_factory_t* factory, const clap_host_t* host, const char* id)
{
    if (strcmp(id, PLUGIN_ID) != 0)
        return NULL;
    Minimal* m = calloc(1, sizeof(Minimal));
    m->host = host;
    m->value = START;
    m->plugin = (clap_plugin_t){&descriptor, m, plugin_init, plugin_destroy, plugin_activate, plugin_deactivate,
                                plugin_start, plugin_stop, plugin_reset, plugin_process, plugin_extension,
                                plugin_main_thread};
    return &m->plugin;
}
static const clap_plugin_factory_t factory = {factory_count, factory_descriptor, factory_create};

static bool entry_init(const char* path) { return true; }
static void entry_deinit(void) {}
static const void* entry_factory(const char* id) { return strcmp(id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &factory : NULL; }

CLAP_EXPORT const clap_plugin_entry_t clap_entry = {CLAP_VERSION_INIT, entry_init, entry_deinit, entry_factory};
