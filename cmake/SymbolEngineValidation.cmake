include_guard(GLOBAL)

include(ExternalProject)

# Every validator that applies to a plugin built with symbolengine_add_plugin, as tests:
#
# - each plugin loaded from its CLAP binary as a host loads it, made, run and destroyed, on every system;
# - CLAP's validator, when SYMBOLENGINE_CLAP_VALIDATOR names it;
# - Steinberg's VST3 validator, built from the same SDK the plugins are built with;
# - pluginval on the VST3 build, when SYMBOLENGINE_PLUGINVAL names it;
# - Apple's auval on the Audio Unit, when SYMBOLENGINE_AUVAL is on. It installs the Audio Unit for the user running
#   the tests, so it is meant for continuous integration.
#
# A validator is a host of its own, so it can't load a plugin built with the sanitizers, which must be the first thing
# a process loads; in a sanitizer build only the loader, built with them too, runs.

option(SYMBOLENGINE_AUVAL "Run auval on Audio Units, which installs them for the user running the tests" OFF)

if(CMAKE_CXX_FLAGS MATCHES "-fsanitize")
    set(SYMBOLENGINE_HOSTS_CAN_LOAD_PLUGINS OFF CACHE INTERNAL "")
else()
    set(SYMBOLENGINE_HOSTS_CAN_LOAD_PLUGINS ON CACHE INTERNAL "")
endif()

add_executable(symbolengine_clap_loader ${PROJECT_SOURCE_DIR}/Tests/ClapLoader/ClapLoader.cpp)
target_link_libraries(symbolengine_clap_loader PRIVATE clap ${CMAKE_DL_LIBS})
symbolengine_set_warnings(symbolengine_clap_loader)

function(symbolengine_vst3_validator out)
    if(NOT TARGET symbolengine_vst3_validator)
        symbolengine_fetch_vst3_sdk()
        set(binary ${CMAKE_BINARY_DIR}/vst3-validator)
        set(arguments
            -DCMAKE_BUILD_TYPE=Release
            -DSMTG_ENABLE_VSTGUI_SUPPORT=OFF
            -DSMTG_ENABLE_VST3_PLUGIN_EXAMPLES=OFF
            -DSMTG_ENABLE_VST3_HOSTING_EXAMPLES=OFF
            -DSMTG_CREATE_PLUGIN_LINK=OFF
            -DSMTG_RUN_VST_VALIDATOR=OFF)
        if(APPLE AND CMAKE_OSX_DEPLOYMENT_TARGET)
            list(APPEND arguments -DCMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET})
        endif()
        if(CMAKE_GENERATOR_PLATFORM)
            set(platform CMAKE_GENERATOR ${CMAKE_GENERATOR} CMAKE_GENERATOR_PLATFORM ${CMAKE_GENERATOR_PLATFORM})
        endif()

        ExternalProject_Add(symbolengine_vst3_validator
            SOURCE_DIR ${SYMBOLENGINE_VST3_SDK_ROOT}
            BINARY_DIR ${binary}
            ${platform}
            CMAKE_ARGS ${arguments}
            BUILD_COMMAND ${CMAKE_COMMAND} --build ${binary} --config Release --target validator
            INSTALL_COMMAND ""
            BUILD_BYPRODUCTS ${binary}/bin/Release/validator${CMAKE_EXECUTABLE_SUFFIX})
    endif()
    set(${out} ${CMAKE_BINARY_DIR}/vst3-validator/bin/Release/validator${CMAKE_EXECUTABLE_SUFFIX} PARENT_SCOPE)
endfunction()

# symbolengine_validate_plugin(<target> [VST3_CLASS_ID <id>])
#
# VST3_CLASS_ID also checks that Steinberg's validator finds the VST3 build under that class ID.
function(symbolengine_validate_plugin target)
    cmake_parse_arguments(PARSE_ARGV 1 validate "" "VST3_CLASS_ID" "")

    get_target_property(formats ${target}_impl SYMBOLENGINE_FORMATS)
    get_target_property(au_codes ${target}_impl SYMBOLENGINE_AU_CODES)

    if(APPLE)
        set(clap_file $<TARGET_BUNDLE_DIR:${target}_clap>)
    else()
        set(clap_file $<TARGET_FILE:${target}_clap>)
    endif()
    add_test(NAME "${target}: loads and processes"
        COMMAND symbolengine_clap_loader $<TARGET_FILE:${target}_clap>)

    if(NOT SYMBOLENGINE_HOSTS_CAN_LOAD_PLUGINS)
        return()
    endif()

    if(SYMBOLENGINE_CLAP_VALIDATOR)
        add_test(NAME "${target}: CLAP validator" COMMAND ${SYMBOLENGINE_CLAP_VALIDATOR} validate ${clap_file})
    endif()

    if(VST3 IN_LIST formats)
        # A VST3 build is a folder on every system; on Windows and Linux its binary is three folders down.
        if(APPLE)
            set(vst3_folder $<TARGET_BUNDLE_DIR:${target}_vst3>)
        else()
            set(vst3_folder
                $<PATH:GET_PARENT_PATH,$<PATH:GET_PARENT_PATH,$<PATH:GET_PARENT_PATH,$<TARGET_FILE:${target}_vst3>>>>)
        endif()

        symbolengine_vst3_validator(validator)
        if(validate_VST3_CLASS_ID)
            add_test(NAME "${target}: VST3 validator"
                COMMAND ${CMAKE_COMMAND} -DPROGRAM=${validator} -DARGUMENT=${vst3_folder}
                    "-DEXPECT=cid = ${validate_VST3_CLASS_ID}"
                    -P ${PROJECT_SOURCE_DIR}/cmake/RunAndExpect.cmake)
        else()
            add_test(NAME "${target}: VST3 validator" COMMAND ${validator} ${vst3_folder})
        endif()

        if(SYMBOLENGINE_PLUGINVAL)
            add_test(NAME "${target}: pluginval VST3"
                COMMAND ${SYMBOLENGINE_PLUGINVAL} --strictness-level 10 --validate-in-process --skip-gui-tests
                    --validate ${vst3_folder})
        endif()
    endif()

    if(APPLE AND SYMBOLENGINE_AUVAL AND AUV2 IN_LIST formats)
        list(GET au_codes 0 type)
        list(GET au_codes 1 subtype)
        list(GET au_codes 2 manufacturer)
        add_test(NAME "${target}: auval"
            COMMAND ${CMAKE_COMMAND} -DCOMPONENT=$<TARGET_BUNDLE_DIR:${target}_auv2> -DTYPE=${type} -DSUBTYPE=${subtype}
                -DMANUFACTURER=${manufacturer} -P ${PROJECT_SOURCE_DIR}/cmake/Auval.cmake)
    endif()
endfunction()
