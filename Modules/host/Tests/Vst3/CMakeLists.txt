# A VST3 module of plugins written straight against Steinberg's SDK, each misbehaving one way, which the host's tests
# load. It is built as a VST3 bundle on every system, laid out as Steinberg's layout says.
set(sdk ${SYMBOLENGINE_VST3_SDK_ROOT})
file(GLOB base_sources ${sdk}/base/source/*.cpp ${sdk}/base/thread/source/*.cpp ${sdk}/pluginterfaces/base/*.cpp)
set(plugin_sdk_sources
    ${base_sources}
    ${sdk}/public.sdk/source/common/commoniids.cpp
    ${sdk}/public.sdk/source/common/commonstringconvert.cpp
    ${sdk}/public.sdk/source/common/memorystream.cpp
    ${sdk}/public.sdk/source/common/pluginview.cpp
    ${sdk}/public.sdk/source/main/moduleinit.cpp
    ${sdk}/public.sdk/source/main/pluginfactory.cpp
    ${sdk}/public.sdk/source/vst/utility/stringconvert.cpp
    ${sdk}/public.sdk/source/vst/vstaudioeffect.cpp
    ${sdk}/public.sdk/source/vst/vstbus.cpp
    ${sdk}/public.sdk/source/vst/vstcomponent.cpp
    ${sdk}/public.sdk/source/vst/vstcomponentbase.cpp
    ${sdk}/public.sdk/source/vst/vsteditcontroller.cpp
    ${sdk}/public.sdk/source/vst/vstinitiids.cpp
    ${sdk}/public.sdk/source/vst/vstnoteexpressiontypes.cpp
    ${sdk}/public.sdk/source/vst/vstparameters.cpp
    ${sdk}/public.sdk/source/vst/vstsinglecomponenteffect.cpp)

if(APPLE)
    list(APPEND plugin_sdk_sources ${sdk}/public.sdk/source/main/macmain.cpp)
elseif(WIN32)
    list(APPEND plugin_sdk_sources ${sdk}/public.sdk/source/main/dllmain.cpp)
else()
    list(APPEND plugin_sdk_sources ${sdk}/public.sdk/source/main/linuxmain.cpp)
endif()

add_library(symbolengine_test_vst3 MODULE TestPlugins.cpp ${plugin_sdk_sources})
target_include_directories(symbolengine_test_vst3 SYSTEM PRIVATE ${sdk})
# The single-component plugins' source leaves the edit controller's to be built on its own, for the two-part ones.
target_compile_definitions(symbolengine_test_vst3 PRIVATE $<IF:$<CONFIG:Debug>,DEVELOPMENT=1,RELEASE=1>
    PROJECT_INCLUDES_VSTEDITCONTROLLER=1 $<$<CXX_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>)
set_source_files_properties(${plugin_sdk_sources} PROPERTIES COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/W0,-w>")
symbolengine_set_warnings(symbolengine_test_vst3)
find_package(Threads REQUIRED)
target_link_libraries(symbolengine_test_vst3 PRIVATE Threads::Threads)
set(name "SymbolEngine Test")

if(APPLE)
    target_link_libraries(symbolengine_test_vst3 PRIVATE "-framework CoreFoundation" "-framework Foundation")
    set_target_properties(symbolengine_test_vst3 PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION vst3
        OUTPUT_NAME ${name}
        MACOSX_BUNDLE_GUI_IDENTIFIER dev.symbolengine.test.vst3
        MACOSX_BUNDLE_BUNDLE_NAME ${name}
        SYMBOLENGINE_BUNDLE "$<TARGET_BUNDLE_DIR:symbolengine_test_vst3>")
else()
    if(WIN32)
        set(architecture x86_64-win)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64")
            set(architecture arm64-win)
        endif()
        set(suffix .vst3)
    else()
        set(architecture x86_64-linux)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
            set(architecture aarch64-linux)
        endif()
        set(suffix .so)
    endif()

    set(bundle "${CMAKE_CURRENT_BINARY_DIR}/${name}.vst3")
    set_target_properties(symbolengine_test_vst3 PROPERTIES
        PREFIX ""
        SUFFIX ${suffix}
        OUTPUT_NAME ${name}
        LIBRARY_OUTPUT_DIRECTORY "$<1:${bundle}/Contents/${architecture}>"
        SYMBOLENGINE_BUNDLE "${bundle}")
endif()

# The scan tests' bundles: one module, each copy's name saying how it misbehaves as its code loads or a plugin of it is
# made, laid out in a folder of their own with the SDK built once for them.
add_library(symbolengine_test_vst3_sdk OBJECT ${plugin_sdk_sources})
target_include_directories(symbolengine_test_vst3_sdk SYSTEM PRIVATE ${sdk})
target_compile_definitions(symbolengine_test_vst3_sdk PRIVATE $<IF:$<CONFIG:Debug>,DEVELOPMENT=1,RELEASE=1>
    PROJECT_INCLUDES_VSTEDITCONTROLLER=1 $<$<CXX_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>)
set_target_properties(symbolengine_test_vst3_sdk PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(symbolengine_misbehaving_vst3 MODULE Misbehaving.cpp $<TARGET_OBJECTS:symbolengine_test_vst3_sdk>)
target_include_directories(symbolengine_misbehaving_vst3 SYSTEM PRIVATE ${sdk})
target_compile_definitions(symbolengine_misbehaving_vst3 PRIVATE $<IF:$<CONFIG:Debug>,DEVELOPMENT=1,RELEASE=1>)
symbolengine_set_warnings(symbolengine_misbehaving_vst3)
target_link_libraries(symbolengine_misbehaving_vst3 PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
set(misbehaving_names "Good A" "Good B" "Good C" "Crash on load" "Abort on load" "Hang on load"
    "Hang on a thread on load" "Crash when made" "Abort when made" "Throw when made" "Overflow when made"
    "Fail fast when made" "Exit when made" "Hang when made" "Many 40 with 20 crashes 50 ms" "Damage" "Slow A 1500 ms"
    "Slow B 1500 ms" "Slow C 1500 ms" "Hungry 300" "Detach deadlock")
list(JOIN misbehaving_names "|" misbehaving_joined)
set(misbehaving_folder "${CMAKE_CURRENT_BINARY_DIR}/Misbehaving")
if(APPLE)
    target_link_libraries(symbolengine_misbehaving_vst3 PRIVATE "-framework CoreFoundation" "-framework Foundation")
    set_target_properties(symbolengine_misbehaving_vst3 PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION vst3
        OUTPUT_NAME "SymbolEngine Misbehaving"
        MACOSX_BUNDLE_GUI_IDENTIFIER dev.symbolengine.misbehaving.vst3
        MACOSX_BUNDLE_BUNDLE_NAME "SymbolEngine Misbehaving")
    set(misbehaving_built "$<TARGET_BUNDLE_DIR:symbolengine_misbehaving_vst3>")
else()
    set_target_properties(symbolengine_misbehaving_vst3 PROPERTIES PREFIX "" SUFFIX ${suffix}
        OUTPUT_NAME "SymbolEngine Misbehaving")
    set(misbehaving_built "$<TARGET_FILE:symbolengine_misbehaving_vst3>")
endif()
add_custom_command(TARGET symbolengine_misbehaving_vst3 POST_BUILD
    COMMAND ${CMAKE_COMMAND} "-DBUILT=${misbehaving_built}" "-DINTO=${misbehaving_folder}"
            "-DNAMES=${misbehaving_joined}" "-DARCHITECTURE=$<IF:$<BOOL:${APPLE}>,,${architecture}>"
            "-DSUFFIX=${suffix}" -P "${CMAKE_CURRENT_SOURCE_DIR}/LayOutBundles.cmake"
    VERBATIM)
set_target_properties(symbolengine_misbehaving_vst3 PROPERTIES SYMBOLENGINE_FOLDER "${misbehaving_folder}")

# A program that loads the module and ends, for the test of the module's entry and exit.
add_executable(symbolengine_host_module_life ModuleLife.cpp)
target_link_libraries(symbolengine_host_module_life PRIVATE SymbolEngine::host)
symbolengine_set_warnings(symbolengine_host_module_life)
