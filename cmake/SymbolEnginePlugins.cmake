include_guard(GLOBAL)

include(FetchContent)

# The CLAP SDK and clap-wrapper, pinned by checksum. Each is fetched only, not added, and clap-wrapper is pointed at
# the CLAP SDK, so it downloads nothing of its own.
FetchContent_Declare(symbolengine_clap
    URL https://github.com/free-audio/clap/archive/refs/tags/1.2.10.tar.gz
    URL_HASH SHA256=58fdb977c6678859c15d57942378b651399178f49a3a8ecad1813b9f231dc096
    SOURCE_SUBDIR fetched-only)
FetchContent_Declare(symbolengine_clap_wrapper
    URL https://github.com/free-audio/clap-wrapper/archive/806bba0a5f792e3966b99c1561ab9952d3316d7c.tar.gz # v0.17.0
    URL_HASH SHA256=5a346f9a651db5ce24a3f1c09cd4b6d71c3e75ca2a6fa79a752db896a92f6cb3
    SOURCE_SUBDIR fetched-only)
FetchContent_MakeAvailable(symbolengine_clap symbolengine_clap_wrapper)

set(CLAP_SDK_ROOT ${symbolengine_clap_SOURCE_DIR})
set(CLAP_WRAPPER_DOWNLOAD_DEPENDENCIES OFF)

# clap-wrapper links its own parts with the static C runtime unless a runtime has been chosen, which clashes with
# everything else in a build left at CMake's default. Choosing CMake's default here keeps them all the same.
if(NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

add_subdirectory(${symbolengine_clap_wrapper_SOURCE_DIR} ${CMAKE_BINARY_DIR}/_deps/symbolengine_clap_wrapper-build
    EXCLUDE_FROM_ALL SYSTEM)

set(SYMBOLENGINE_PLUGIN_ENTRY ${PROJECT_SOURCE_DIR}/Modules/plugin/entry/ClapEntry.cpp
    CACHE INTERNAL "The source of the entry point compiled into each plugin")

# symbolengine_add_plugin(<target>
#     SOURCES <files...>
#     OUTPUT_NAME <name>              # the plugin's file name, such as "Gain"
#     BUNDLE_IDENTIFIER <identifier>  # such as com.example.gain
#     [VERSION <version>]
#     [FORMATS CLAP ...]              # CLAP for now
#     [LIBRARIES <libraries...>])
#
# Builds the plugins listed in the sources' SYMBOLENGINE_PLUGINS. The plugins themselves go in <target>_impl, a
# static library, and each format's build of it is <target>_clap and so on, all in <build>/Plugins/<target>.
function(symbolengine_add_plugin target)
    cmake_parse_arguments(PARSE_ARGV 1 plugin "" "OUTPUT_NAME;BUNDLE_IDENTIFIER;VERSION" "SOURCES;FORMATS;LIBRARIES")

    if(NOT plugin_OUTPUT_NAME)
        set(plugin_OUTPUT_NAME ${target})
    endif()
    if(NOT plugin_VERSION)
        set(plugin_VERSION ${PROJECT_VERSION})
    endif()
    if(NOT plugin_FORMATS)
        set(plugin_FORMATS CLAP)
    endif()
    if(NOT plugin_BUNDLE_IDENTIFIER)
        message(FATAL_ERROR "symbolengine_add_plugin(${target}) needs a BUNDLE_IDENTIFIER, such as com.example.gain")
    endif()

    set(implementation ${target}_impl)
    add_library(${implementation} STATIC ${plugin_SOURCES})
    target_link_libraries(${implementation} PUBLIC SymbolEngine::plugin ${plugin_LIBRARIES})

    # Two plugins built on different versions can be loaded into one host, so nothing of either is visible outside
    # its own binary except the entry point.
    set_target_properties(${implementation} PROPERTIES
        CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON POSITION_INDEPENDENT_CODE ON)

    make_clapfirst_plugins(
        TARGET_NAME ${target}
        IMPL_TARGET ${implementation}
        OUTPUT_NAME "${plugin_OUTPUT_NAME}"
        ENTRY_SOURCE ${SYMBOLENGINE_PLUGIN_ENTRY}
        BUNDLE_IDENTIFIER ${plugin_BUNDLE_IDENTIFIER}
        BUNDLE_VERSION ${plugin_VERSION}
        COPY_AFTER_BUILD FALSE
        ASSET_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Plugins/${target}
        PLUGIN_FORMATS ${plugin_FORMATS})
endfunction()
