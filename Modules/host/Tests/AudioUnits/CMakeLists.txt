# The host's test Audio Units: one component holding many units, built on Apple's AudioUnitSDK, each misbehaving one
# way, which the host's tests register with their own process, never installing them.
include(SymbolEngineAudioUnitSdk)
symbolengine_fetch_audiounit_sdk()
set(sdk ${SYMBOLENGINE_AUDIOUNIT_SDK_ROOT})
file(GLOB sdk_sources ${sdk}/src/AudioUnitSDK/*.cpp)
set_source_files_properties(${sdk_sources} PROPERTIES COMPILE_OPTIONS -w)

set(frameworks "-framework AppKit" "-framework AudioToolbox" "-framework CoreAudio" "-framework CoreFoundation"
               "-framework Foundation")

add_library(symbolengine_test_audio_units MODULE TestUnits.cpp TestUnitViews.mm ${sdk_sources})
target_include_directories(symbolengine_test_audio_units SYSTEM PRIVATE ${sdk}/include)
target_link_libraries(symbolengine_test_audio_units PRIVATE ${frameworks})
symbolengine_set_warnings(symbolengine_test_audio_units)
set_target_properties(symbolengine_test_audio_units PROPERTIES
    BUNDLE TRUE
    BUNDLE_EXTENSION component
    OUTPUT_NAME "SymbolEngine Test Units"
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in
    SYMBOLENGINE_BUNDLE "$<TARGET_BUNDLE_DIR:symbolengine_test_audio_units>")

# A second component built from the same source, declaring one unit whose view names a class the first component's
# copy of the views has taken in a process that loaded both, as two plugins built on one framework do.
add_library(symbolengine_test_audio_units_twin MODULE TestUnits.cpp TestUnitViews.mm ${sdk_sources})
target_include_directories(symbolengine_test_audio_units_twin SYSTEM PRIVATE ${sdk}/include)
target_link_libraries(symbolengine_test_audio_units_twin PRIVATE ${frameworks})
symbolengine_set_warnings(symbolengine_test_audio_units_twin)
set_target_properties(symbolengine_test_audio_units_twin PROPERTIES
    BUNDLE TRUE
    BUNDLE_EXTENSION component
    OUTPUT_NAME "SymbolEngine Twin Test Units"
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/TwinInfo.plist.in)

# The same units built only for Intel under a maker code of their own, for the test of a unit the system runs through
# Rosetta, which installs them in the user's Components folder while it runs: only on a machine made for the purpose.
if(SYMBOLENGINE_TEST_INTEL_AUDIO_UNITS)
    file(READ Info.plist.in plist)
    string(REPLACE "<string>SyTs</string>" "<string>SyTi</string>" plist "${plist}")
    string(REPLACE "dev.symbolengine.test.units" "dev.symbolengine.test.units.intel" plist "${plist}")
    string(REPLACE "<string>SymbolEngine Test Units</string>" "<string>SymbolEngine Intel Test Units</string>" plist
                   "${plist}")
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/IntelInfo.plist.in "${plist}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS Info.plist.in)

    add_library(symbolengine_test_audio_units_intel MODULE TestUnits.cpp TestUnitViews.mm ${sdk_sources})
    target_include_directories(symbolengine_test_audio_units_intel SYSTEM PRIVATE ${sdk}/include)
    target_link_libraries(symbolengine_test_audio_units_intel PRIVATE ${frameworks})
    symbolengine_set_warnings(symbolengine_test_audio_units_intel)
    set_target_properties(symbolengine_test_audio_units_intel PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION component
        OUTPUT_NAME "SymbolEngine Intel Test Units"
        OSX_ARCHITECTURES x86_64
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_BINARY_DIR}/IntelInfo.plist.in)
endif()
