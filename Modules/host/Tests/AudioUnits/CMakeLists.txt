# The host's test Audio Units: one component holding many units, built on Apple's AudioUnitSDK, each misbehaving one
# way, which the host's tests register with their own process, never installing them.
include(SymbolEngineAudioUnitSdk)
symbolengine_fetch_audiounit_sdk()
set(sdk ${SYMBOLENGINE_AUDIOUNIT_SDK_ROOT})
file(GLOB sdk_sources ${sdk}/src/AudioUnitSDK/*.cpp)
set_source_files_properties(${sdk_sources} PROPERTIES COMPILE_OPTIONS -w)

add_library(symbolengine_test_audio_units MODULE TestUnits.cpp ${sdk_sources})
target_include_directories(symbolengine_test_audio_units SYSTEM PRIVATE ${sdk}/include)
target_link_libraries(symbolengine_test_audio_units PRIVATE "-framework AudioToolbox" "-framework CoreAudio"
                                                            "-framework CoreFoundation" "-framework Foundation")
symbolengine_set_warnings(symbolengine_test_audio_units)
set_target_properties(symbolengine_test_audio_units PROPERTIES
    BUNDLE TRUE
    BUNDLE_EXTENSION component
    OUTPUT_NAME "SymbolEngine Test Units"
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in
    SYMBOLENGINE_BUNDLE "$<TARGET_BUNDLE_DIR:symbolengine_test_audio_units>")
