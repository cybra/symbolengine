# cmake -DFILE=<src/wrapasvst3.cpp> -P PatchClapWrapper.cmake
#
# When a VST3 host asks for a speaker arrangement a plugin refuses, clap-wrapper asks again by channel count alone, for
# plugins that can't read a channel map. A plugin that describes its own speakers has read the map and refused it, and
# taking the count instead would leave the host naming its channels for speakers the plugin isn't feeding, so it is
# asked again only when it doesn't. The change looks for the text it replaces and stops the build if clap-wrapper has
# changed it, so a new version is never patched wrongly.

file(READ "${FILE}" source)

if(source MATCHES "!_plugin->_ext._surround && apply\\(undescribed\\)")
    return()
endif()

macro(replace_once old new)
    string(FIND "${source}" "${old}" at)
    if(at EQUAL -1)
        message(FATAL_ERROR "PatchClapWrapper.cmake: clap-wrapper's VST3 wrapper has changed; this text is gone: ${old}")
    endif()
    string(REPLACE "${old}" "${new}" source "${source}")
endmacro()

replace_once([[    // a plugin that does not know CLAP_PORT_SURROUND may refuse the described set; the
    // channel counts on their own are still worth asking for, as they always were
    if (apply(requests) || (describesLayout && apply(undescribed)))]]
    [[    // a plugin that does not know CLAP_PORT_SURROUND may refuse the described set; the
    // channel counts on their own are still worth asking for, as they always were, but not
    // of a plugin with clap.surround, which read the layout and refused it
    if (apply(requests) || (describesLayout && !_plugin->_ext._surround && apply(undescribed)))]])

file(WRITE "${FILE}" "${source}")
