// The entry point a host finds in a plugin's binary, compiled into each format's build of it. It hands the host the
// plugin module's factory.

#include <clap/clap.h>

extern "C"
{
    bool symbolengine_clap_init(const char* path);
    void symbolengine_clap_deinit(void);
    const void* symbolengine_clap_get_factory(const char* id);

    CLAP_EXPORT extern const clap_plugin_entry clap_entry;

    const clap_plugin_entry clap_entry = {CLAP_VERSION_INIT, symbolengine_clap_init, symbolengine_clap_deinit,
                                          symbolengine_clap_get_factory};
}
