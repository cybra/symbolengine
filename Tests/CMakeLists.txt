# A project outside SymbolEngine takes it in both ways the README describes: FetchContent from its source, and
# find_package once it is installed. Each builds Tests/Consumer and runs what it built.

set(consumer_source ${CMAKE_CURRENT_SOURCE_DIR}/Consumer)
set(consumer_build ${CMAKE_CURRENT_BINARY_DIR}/consumer)

set(consumer_options
    -DCMAKE_BUILD_TYPE=$<CONFIG>
    -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
    "-DCMAKE_CXX_FLAGS=${CMAKE_CXX_FLAGS}"
    "-DCMAKE_EXE_LINKER_FLAGS=${CMAKE_EXE_LINKER_FLAGS}")

if(DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
    list(APPEND consumer_options "-DCMAKE_MSVC_RUNTIME_LIBRARY=${CMAKE_MSVC_RUNTIME_LIBRARY}")
endif()

# The consumer builds a plugin too when the plugin module is on, from the SDKs this build already fetched.
if(SYMBOLENGINE_PLUGIN)
    FetchContent_GetProperties(symbolengine_clap)
    FetchContent_GetProperties(symbolengine_clap_wrapper)
    list(APPEND consumer_options
        -DFETCHCONTENT_SOURCE_DIR_SYMBOLENGINE_CLAP=${symbolengine_clap_SOURCE_DIR}
        -DFETCHCONTENT_SOURCE_DIR_SYMBOLENGINE_CLAP_WRAPPER=${symbolengine_clap_wrapper_SOURCE_DIR})
else()
    list(APPEND consumer_options -DSYMBOLENGINE_PLUGIN=OFF)
endif()

if(APPLE AND CMAKE_OSX_DEPLOYMENT_TARGET)
    list(APPEND consumer_options -DCMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET})
endif()

set(consumer_generator --build-generator ${CMAKE_GENERATOR})
if(CMAKE_GENERATOR_PLATFORM)
    list(APPEND consumer_generator --build-generator-platform ${CMAKE_GENERATOR_PLATFORM})
endif()
if(CMAKE_MAKE_PROGRAM)
    list(APPEND consumer_generator --build-makeprogram ${CMAKE_MAKE_PROGRAM})
endif()

add_test(NAME "consumer: FetchContent"
    COMMAND ${CMAKE_CTEST_COMMAND}
        --build-and-test ${consumer_source} ${consumer_build}/fetch
        ${consumer_generator}
        --build-config $<CONFIG>
        --build-options ${consumer_options} -DCONSUME=fetch -DSYMBOLENGINE_SOURCE=${PROJECT_SOURCE_DIR}
        --test-command consumer)

add_test(NAME "consumer: install"
    COMMAND ${CMAKE_COMMAND} --install ${PROJECT_BINARY_DIR} --prefix ${consumer_build}/prefix --config $<CONFIG>)
set_tests_properties("consumer: install" PROPERTIES FIXTURES_SETUP installed)

add_test(NAME "consumer: find_package"
    COMMAND ${CMAKE_CTEST_COMMAND}
        --build-and-test ${consumer_source} ${consumer_build}/package
        ${consumer_generator}
        --build-config $<CONFIG>
        --build-options ${consumer_options} -DCONSUME=package -DCMAKE_PREFIX_PATH=${consumer_build}/prefix
        --test-command consumer)
set_tests_properties("consumer: find_package" PROPERTIES FIXTURES_REQUIRED installed)

# A plugin that states its VST3 class ID, as one moved from elsewhere does, found by Steinberg's validator under it,
# and takes the speaker arrangements its layouts allow: any standard one out, from mono or the same one in.
if(SYMBOLENGINE_PLUGIN)
    symbolengine_add_plugin(IdentityTest
        SOURCES IdentityPlugin/IdentityPlugin.cpp
        BUNDLE_IDENTIFIER test.symbolengine.identity
        FORMATS CLAP VST3)
    symbolengine_set_warnings(IdentityTest_impl)
    symbolengine_validate_plugin(IdentityTest VST3_CLASS_ID ABCDEF0153796D624964656E74697479
        VST3_ARRANGEMENTS Mono>Mono Mono>Stereo Mono>30Cine Mono>40Music Mono>50
            Stereo>Stereo 30Cine>30Cine 40Music>40Music 50>50)
endif()

# clap-wrapper's VST3 speaker arrangements, from SymbolEngine's fork, checked on their own: Dolby's wide pair has
# numbers only for a plugin that takes it.
if(SYMBOLENGINE_PLUGIN AND SYMBOLENGINE_VST3_SDK_ROOT)
    FetchContent_GetProperties(symbolengine_clap_wrapper)
    add_executable(symbolengine_vst3_speakers Vst3Speakers/Vst3SpeakersTests.cpp
        ${symbolengine_clap_wrapper_SOURCE_DIR}/src/detail/vst3/channelmap.cpp)
    target_include_directories(symbolengine_vst3_speakers SYSTEM PRIVATE ${SYMBOLENGINE_VST3_SDK_ROOT}
        ${symbolengine_clap_wrapper_SOURCE_DIR}/src)
    target_link_libraries(symbolengine_vst3_speakers PRIVATE clap clap-wrapper-extensions Catch2::Catch2WithMain)
    catch_discover_tests(symbolengine_vst3_speakers TEST_PREFIX "clap-wrapper: " DISCOVERY_MODE PRE_TEST)
endif()

# A plugin that marks each channel with the speaker it is for. On a Mac a host in the test itself runs its Audio Unit
# in the layouts hosts set, Logic's channel orders among them, and in channel counts alone, and checks that every
# channel reaches the plugin as its speaker and comes back where it went in.
if(SYMBOLENGINE_PLUGIN)
    symbolengine_add_plugin(SpeakersTest
        SOURCES SpeakersPlugin/SpeakersPlugin.cpp
        BUNDLE_IDENTIFIER test.symbolengine.speakers
        FORMATS CLAP VST3 AUV2
        AU_SUBTYPE Spkr
        AU_MANUFACTURER_CODE SyEn
        AU_MANUFACTURER_NAME SymbolEngine)
    symbolengine_set_warnings(SpeakersTest_impl)
    symbolengine_validate_plugin(SpeakersTest)

    get_target_property(speakers_formats SpeakersTest_impl SYMBOLENGINE_FORMATS)
    if(APPLE AND SYMBOLENGINE_HOSTS_CAN_LOAD_PLUGINS AND AUV2 IN_LIST speakers_formats)
        add_executable(symbolengine_audio_unit_layouts AudioUnitLayouts/AudioUnitLayouts.cpp)
        target_link_libraries(symbolengine_audio_unit_layouts PRIVATE "-framework AudioToolbox"
            "-framework CoreFoundation")
        symbolengine_set_warnings(symbolengine_audio_unit_layouts)
        add_test(NAME "SpeakersTest: Audio Unit layouts"
            COMMAND symbolengine_audio_unit_layouts $<TARGET_BUNDLE_DIR:SpeakersTest_auv2>)
    endif()
endif()
