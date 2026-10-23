include_guard(GLOBAL)

include(FetchContent)

# The CLAP SDK and clap-wrapper, pinned by checksum. Each is fetched only, not added, and clap-wrapper is pointed at
# the CLAP SDK, so it downloads nothing of its own. clap-wrapper comes from SymbolEngine's fork, whose symbolengine
# branch adds its changes to a clap-wrapper release, each a commit of its own: VST3 speaker arrangements a plugin
# refuses stay refused, Audio Units have channel layouts, and Dolby's wide speakers pass between a plugin and the VST3
# and Audio Unit wrappers.
FetchContent_Declare(symbolengine_clap
    URL https://github.com/free-audio/clap/archive/refs/tags/1.2.10.tar.gz
    URL_HASH SHA256=58fdb977c6678859c15d57942378b651399178f49a3a8ecad1813b9f231dc096
    SOURCE_SUBDIR fetched-only)
FetchContent_Declare(symbolengine_clap_wrapper
    URL https://github.com/cybra/clap-wrapper/archive/8c30544e82fab0bb0e9be705b15e497c6d657cf3.tar.gz # v0.17.0 and ours
    URL_HASH SHA256=e65c2b02677b5392b18e17ea061f36c8e1c7a90ac10c550698e5b193b38ab49e
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

# The VST3 SDK and Apple's AudioUnitSDK, at the versions clap-wrapper is tested with, fetched the first time a
# plugin is built in a format that needs them. The VST3 SDK's archive leaves its parts empty, so each is fetched into
# its place. A developer's own copy can be used instead by setting VST3_SDK_ROOT or AUDIOUNIT_SDK_ROOT.
function(symbolengine_fetch_vst3_sdk)
    if(SYMBOLENGINE_VST3_SDK_ROOT)
        return()
    endif()
    if(VST3_SDK_ROOT)
        set(SYMBOLENGINE_VST3_SDK_ROOT ${VST3_SDK_ROOT} CACHE INTERNAL "")
        return()
    endif()

    FetchContent_Declare(symbolengine_vst3sdk
        URL https://github.com/steinbergmedia/vst3sdk/archive/9fad9770f2ae8542ab1a548a68c1ad1ac690abe0.tar.gz # v3.8.0_build_66
        URL_HASH SHA256=121e1063962aea6e02817fec6bd3066162e22f262904c00b4be4e8a1fb3f826b
        SOURCE_SUBDIR fetched-only)
    FetchContent_MakeAvailable(symbolengine_vst3sdk)

    set(parts
        base 3d2e82f8e6bff59c1d8b7a27491a29c2286b5206 8f1d5f9ac0cd1e916ca3a196f5ee080ec4301b0b10a7ee7e564013f5d58b03cd
        cmake de6e54eeaaab35b7145f5c32c279b5e892146e04 ab4ce274563570fed11f2f0a31feb93280cd26543703f90c404a8720c1a486a5
        pluginterfaces 31d6eeba6daaa3e2a8bfbe3e7a90ca0b7fbfbc1c 7c9d19af0e81711edde34c3eb2e5e6d150ae1f449501bb5b352605b9590654d7
        public_sdk a3911a4615dabbfdfd9d181ee26b05c70c289a95 4cc8a9a57a970172b1efbef62025ae4f0656e132eed994f0e15ed19dfd9b0bb3)

    while(parts)
        list(POP_FRONT parts part commit hash)
        string(REPLACE "_" "." folder ${part})
        FetchContent_Declare(symbolengine_vst3_${part}
            URL https://github.com/steinbergmedia/vst3_${part}/archive/${commit}.tar.gz
            URL_HASH SHA256=${hash}
            SOURCE_DIR ${symbolengine_vst3sdk_SOURCE_DIR}/${folder}
            SOURCE_SUBDIR fetched-only)
        FetchContent_MakeAvailable(symbolengine_vst3_${part})
    endwhile()

    set(SYMBOLENGINE_VST3_SDK_ROOT ${symbolengine_vst3sdk_SOURCE_DIR} CACHE INTERNAL "")
endfunction()

function(symbolengine_fetch_audiounit_sdk)
    if(SYMBOLENGINE_AUDIOUNIT_SDK_ROOT)
        return()
    endif()
    if(AUDIOUNIT_SDK_ROOT)
        set(SYMBOLENGINE_AUDIOUNIT_SDK_ROOT ${AUDIOUNIT_SDK_ROOT} CACHE INTERNAL "")
        return()
    endif()

    FetchContent_Declare(symbolengine_audiounit_sdk
        URL https://github.com/apple/AudioUnitSDK/archive/dca901da743f491cf25c0ec2503b1373c7f9f69c.tar.gz # AudioUnitSDK-1.1.0
        URL_HASH SHA256=602aba0094bf179cf587eae4b8c585c2cfa996ff1a33a9f8522e14414272a890
        SOURCE_SUBDIR fetched-only)
    FetchContent_MakeAvailable(symbolengine_audiounit_sdk)

    set(SYMBOLENGINE_AUDIOUNIT_SDK_ROOT ${symbolengine_audiounit_sdk_SOURCE_DIR} CACHE INTERNAL "")
endfunction()

set(SYMBOLENGINE_CODESIGN_IDENTITY "-" CACHE STRING
    "The identity plugin bundles are signed with on a Mac, such as a Developer ID Application; - signs them ad hoc")

set(SYMBOLENGINE_PLUGIN_ENTRY ${PROJECT_SOURCE_DIR}/Modules/plugin/entry/ClapEntry.cpp
    CACHE INTERNAL "The source of the entry point compiled into each plugin")

# symbolengine_add_plugin(<target>
#     SOURCES <files...>
#     OUTPUT_NAME <name>              # the plugin's file name, such as "Gain"
#     BUNDLE_IDENTIFIER <identifier>  # such as com.example.gain
#     [VERSION <version>]
#     [FORMATS CLAP VST3 AUV2]        # CLAP alone if not given; AUV2 is built on a Mac only
#     [AU_TYPE <code>]                # aufx for an effect (the default), aumf for an effect that takes notes,
#                                     # aumu for an instrument
#     [AU_SUBTYPE <code>]             # four characters, the plugin's own among its maker's
#     [AU_MANUFACTURER_CODE <code>]   # four characters with at least one capital, the maker's
#     [AU_MANUFACTURER_NAME <name>]
#     [LIBRARIES <libraries...>])
#
# Builds the plugins listed in the sources' SYMBOLENGINE_PLUGINS; a build with the sanitizers makes the CLAP build
# alone. The plugins themselves go in <target>_impl, a
# static library, and each format's build of it is <target>_clap, <target>_vst3 and <target>_auv2, all in
# <build>/Plugins/<target>. An Audio Unit's codes are its identity in every host and session, as a VST3 plugin's
# class ID is, so neither may change once the plugin is released.
function(symbolengine_add_plugin target)
    cmake_parse_arguments(PARSE_ARGV 1 plugin ""
        "OUTPUT_NAME;BUNDLE_IDENTIFIER;VERSION;AU_TYPE;AU_SUBTYPE;AU_MANUFACTURER_CODE;AU_MANUFACTURER_NAME"
        "SOURCES;FORMATS;LIBRARIES")

    if(NOT plugin_OUTPUT_NAME)
        set(plugin_OUTPUT_NAME ${target})
    endif()
    if(NOT plugin_VERSION)
        set(plugin_VERSION ${PROJECT_VERSION})
    endif()
    if(NOT plugin_FORMATS)
        set(plugin_FORMATS CLAP)
    endif()
    if(NOT plugin_AU_TYPE)
        set(plugin_AU_TYPE aufx)
    endif()
    if(NOT plugin_AU_MANUFACTURER_NAME)
        set(plugin_AU_MANUFACTURER_NAME ${plugin_OUTPUT_NAME})
    endif()
    if(NOT plugin_BUNDLE_IDENTIFIER)
        message(FATAL_ERROR "symbolengine_add_plugin(${target}) needs a BUNDLE_IDENTIFIER, such as com.example.gain")
    endif()

    if(NOT APPLE)
        list(REMOVE_ITEM plugin_FORMATS AUV2)
    endif()

    # A plugin built with the sanitizers leaves their runtime for the program that loads it to bring, which the
    # other formats' wrappers refuse to link, so such a build makes the CLAP build alone.
    if(CMAKE_CXX_FLAGS MATCHES "-fsanitize")
        set(plugin_FORMATS CLAP)
    endif()

    set(au_arguments)
    if(AUV2 IN_LIST plugin_FORMATS)
        foreach(code IN ITEMS AU_TYPE AU_SUBTYPE AU_MANUFACTURER_CODE)
            string(LENGTH "${plugin_${code}}" length)
            if(NOT length EQUAL 4)
                message(FATAL_ERROR "symbolengine_add_plugin(${target}) builds an Audio Unit, so ${code} must be four characters")
            endif()
        endforeach()
        if(NOT plugin_AU_MANUFACTURER_CODE MATCHES "[A-Z]")
            message(FATAL_ERROR "symbolengine_add_plugin(${target}): an Audio Unit's manufacturer code needs a capital letter")
        endif()

        symbolengine_fetch_audiounit_sdk()
        set(AUDIOUNIT_SDK_ROOT ${SYMBOLENGINE_AUDIOUNIT_SDK_ROOT})
        set(au_arguments
            AUV2_MANUFACTURER_NAME "${plugin_AU_MANUFACTURER_NAME}"
            AUV2_MANUFACTURER_CODE "${plugin_AU_MANUFACTURER_CODE}"
            AUV2_SUBTYPE_CODE "${plugin_AU_SUBTYPE}"
            AUV2_INSTRUMENT_TYPE "${plugin_AU_TYPE}")
    endif()

    if(VST3 IN_LIST plugin_FORMATS)
        symbolengine_fetch_vst3_sdk()
        set(VST3_SDK_ROOT ${SYMBOLENGINE_VST3_SDK_ROOT})
    endif()

    set(implementation ${target}_impl)
    add_library(${implementation} STATIC ${plugin_SOURCES})
    target_link_libraries(${implementation} PUBLIC SymbolEngine::plugin ${plugin_LIBRARIES})

    # Two plugins built on different versions can be loaded into one host, so nothing of either is visible outside
    # its own binary except the entry point.
    set_target_properties(${implementation} PROPERTIES
        CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON POSITION_INDEPENDENT_CODE ON)

    # clap-wrapper builds the formats' wrappers here, where they must be position independent too to link into a
    # plugin, and hidden, as everything in one is.
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    set(CMAKE_CXX_VISIBILITY_PRESET hidden)
    set(CMAKE_OBJCXX_VISIBILITY_PRESET hidden)
    set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

    # A VST3 plugin on Windows is a folder, as on the other systems, which Steinberg's validator expects.
    make_clapfirst_plugins(
        TARGET_NAME ${target}
        IMPL_TARGET ${implementation}
        OUTPUT_NAME "${plugin_OUTPUT_NAME}"
        ENTRY_SOURCE ${SYMBOLENGINE_PLUGIN_ENTRY}
        BUNDLE_IDENTIFIER ${plugin_BUNDLE_IDENTIFIER}
        BUNDLE_VERSION ${plugin_VERSION}
        COPY_AFTER_BUILD FALSE
        WINDOWS_FOLDER_VST3 TRUE
        ASSET_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Plugins/${target}
        PLUGIN_FORMATS ${plugin_FORMATS}
        ${au_arguments})

    # clap-wrapper puts the Audio Unit's Info.plist, which names it to macOS, into its bundle only when the bundle
    # links, and configuring the project writes CMake's own over it; until the next link macOS can't find the Audio
    # Unit. Copying it in after every build keeps it.
    if(AUV2 IN_LIST plugin_FORMATS)
        add_custom_target(${target}_auv2_info ALL
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_CURRENT_BINARY_DIR}/${target}_auv2-build-helper-output/auv2_Info.plist
                $<TARGET_BUNDLE_CONTENT_DIR:${target}_auv2>/Info.plist
            VERBATIM)
        add_dependencies(${target}_auv2_info ${target}_auv2)
        add_dependencies(${target}_all ${target}_auv2_info)
    endif()

    # On a Mac the linker signs only the binary, and a host that checks the whole bundle, as Live does with a VST3,
    # refuses it. Each bundle is signed whole after every build, after the Audio Unit's Info.plist is back in place.
    if(APPLE)
        set(bundles)
        foreach(format IN LISTS plugin_FORMATS)
            string(TOLOWER ${format} suffix)
            list(APPEND bundles $<TARGET_BUNDLE_DIR:${target}_${suffix}>)
        endforeach()
        add_custom_target(${target}_sign ALL
            COMMAND codesign --force --sign ${SYMBOLENGINE_CODESIGN_IDENTITY} ${bundles}
            COMMENT "Signing ${target}'s plugins"
            VERBATIM)
        foreach(format IN LISTS plugin_FORMATS)
            string(TOLOWER ${format} suffix)
            add_dependencies(${target}_sign ${target}_${suffix})
        endforeach()
        if(TARGET ${target}_auv2_info)
            add_dependencies(${target}_sign ${target}_auv2_info)
        endif()
        add_dependencies(${target}_all ${target}_sign)
    endif()

    set_target_properties(${implementation} PROPERTIES
        SYMBOLENGINE_FORMATS "${plugin_FORMATS}"
        SYMBOLENGINE_AU_CODES "${plugin_AU_TYPE};${plugin_AU_SUBTYPE};${plugin_AU_MANUFACTURER_CODE}")
endfunction()
