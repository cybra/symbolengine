# The host's watcher thread and the tests' audio threads.
find_package(Threads REQUIRED)

# Steinberg's VST3 SDK, MIT licensed: its interfaces, and the few of its hosting classes the host uses as they are,
# its host context, messages, parameter queues and event lists, and the identities of its editors' interfaces. Its
# sources build into the module with their own warnings left as Steinberg has them.
include(SymbolEngineVst3Sdk)
symbolengine_fetch_vst3_sdk()
set(sdk ${SYMBOLENGINE_VST3_SDK_ROOT})
set(sdk_sources
    ${sdk}/pluginterfaces/base/conststringtable.cpp
    ${sdk}/pluginterfaces/base/coreiids.cpp
    ${sdk}/pluginterfaces/base/funknown.cpp
    ${sdk}/pluginterfaces/base/ustring.cpp
    ${sdk}/public.sdk/source/common/commoniids.cpp
    ${sdk}/public.sdk/source/common/commonstringconvert.cpp
    ${sdk}/public.sdk/source/vst/hosting/eventlist.cpp
    ${sdk}/public.sdk/source/vst/hosting/hostclasses.cpp
    ${sdk}/public.sdk/source/vst/hosting/pluginterfacesupport.cpp
    ${sdk}/public.sdk/source/vst/utility/stringconvert.cpp
    ${sdk}/public.sdk/source/vst/vstinitiids.cpp)
set_source_files_properties(${sdk_sources} PROPERTIES COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/W0,-w>")

set(sources
    src/FakePluginFormat.cpp
    src/Guard.cpp
    src/Plugin.cpp
    src/PluginFiles.cpp
    src/PluginFormat.cpp
    src/PluginList.cpp
    src/PluginScanWorker.cpp
    src/PluginScanner.cpp
    src/ProcessState.cpp
    src/Vst3Format.cpp
    src/Vst3Module.cpp
    ${sdk_sources})
set(libraries Threads::Threads)

if(APPLE)
    list(APPEND sources src/AudioUnitFormat.mm src/MainThread_other.cpp src/ProcessState_mac.mm
                        src/ScanWorkerCrash_posix.cpp src/ScanWorkerLoop_mac.mm src/Vst3Module_mac.cpp
                        src/Vst3View_mac.mm)
    list(APPEND libraries "-framework AppKit" "-framework AudioToolbox" "-framework CoreAudio" "-framework CoreFoundation"
                          "-framework CoreGraphics")
elseif(WIN32)
    list(APPEND sources src/MainThread_windows.cpp src/ScanWorkerCrash_windows.cpp src/ScanWorkerLoop_windows.cpp
                        src/Vst3Module_windows.cpp src/Vst3View_windows.cpp)
    list(APPEND libraries ole32 user32 version)
else()
    list(APPEND sources src/MainThread_other.cpp src/ScanWorkerCrash_posix.cpp src/ScanWorkerLoop_other.cpp
                        src/Vst3Module_linux.cpp src/Vst3View_other.cpp)
    list(APPEND libraries ${CMAKE_DL_LIBS})
endif()

# A plugin's own window, built where the ui module's windows are.
set(dependencies core)
if(TARGET symbolengine_ui)
    list(APPEND sources src/PluginWindow.cpp src/PluginWindows.cpp
                        $<IF:$<BOOL:${APPLE}>,src/PluginWindow_mac.mm,src/PluginWindow_windows.cpp>
                        $<IF:$<BOOL:${APPLE}>,src/PluginWindows_mac.mm,src/PluginWindows_windows.cpp>)
    list(APPEND dependencies ui)
endif()

symbolengine_add_module(host SOURCES ${sources} DEPENDS ${dependencies} LIBRARIES ${libraries})
if(TARGET symbolengine_ui)
    # SymbolEngine's own page of a plugin's parameters, shown where it has no editor.
    include(SymbolEngineResources)
    symbolengine_add_files(symbolengine_host FOLDER web FUNCTION symbolengine_host_parameter_page)
endif()
target_include_directories(symbolengine_host SYSTEM PRIVATE ${sdk})

if(SYMBOLENGINE_TESTS)
    add_subdirectory(Tests/Vst3)

    # The program the scan worker tests start as a worker, and as a program with a worker of its own.
    add_executable(symbolengine_host_scan_worker Tests/ScanWorker.cpp)
    target_link_libraries(symbolengine_host_scan_worker PRIVATE SymbolEngine::host)
    if(APPLE)
        target_link_libraries(symbolengine_host_scan_worker PRIVATE "-framework AudioToolbox"
                                                                    "-framework CoreFoundation")
    endif()
    symbolengine_set_warnings(symbolengine_host_scan_worker)

    set(test_sources Tests/Allocations.cpp Tests/EditorTests.cpp Tests/ParameterTests.cpp Tests/PluginLifeTests.cpp
                     Tests/PluginListTests.cpp Tests/ScanTests.cpp Tests/ScanWorkerTests.cpp Tests/TimeTests.cpp
                     Tests/Vst3Tests.cpp Tests/WalkTests.cpp)
    if(APPLE)
        add_subdirectory(Tests/AudioUnits)
        list(APPEND test_sources Tests/AudioUnitTests.cpp Tests/ScanWorkerTests_mac.mm)
    endif()
    if(TARGET symbolengine_ui)
        list(APPEND test_sources $<IF:$<BOOL:${APPLE}>,Tests/EditorTests_mac.mm,Tests/EditorTests_windows.cpp>)
    endif()
    symbolengine_add_tests(host SOURCES ${test_sources} ALONE "[timing],[windows]")
    target_include_directories(symbolengine_host_tests PRIVATE ${PROJECT_SOURCE_DIR}/Modules/core/Tests)
    if(TARGET symbolengine_ui)
        target_compile_definitions(symbolengine_host_tests PRIVATE SYMBOLENGINE_PLUGIN_WINDOWS=1)
    endif()
    if(APPLE)
        target_link_libraries(symbolengine_host_tests PRIVATE "-framework AppKit" "-framework AudioToolbox")
        target_compile_definitions(symbolengine_host_tests PRIVATE
            "SYMBOLENGINE_TEST_AUDIO_UNITS=\"$<TARGET_BUNDLE_DIR:symbolengine_test_audio_units>\""
            "SYMBOLENGINE_TEST_TWIN_AUDIO_UNITS=\"$<TARGET_BUNDLE_DIR:symbolengine_test_audio_units_twin>\""
            "SYMBOLENGINE_TEST_AU_LOG=\"${CMAKE_CURRENT_BINARY_DIR}/test-au-log.txt\"")
        add_dependencies(symbolengine_host_tests symbolengine_test_audio_units symbolengine_test_audio_units_twin)
        if(SYMBOLENGINE_TEST_INTEL_AUDIO_UNITS)
            target_compile_definitions(symbolengine_host_tests PRIVATE
                "SYMBOLENGINE_TEST_INTEL_AUDIO_UNITS=\"$<TARGET_BUNDLE_DIR:symbolengine_test_audio_units_intel>\"")
            add_dependencies(symbolengine_host_tests symbolengine_test_audio_units_intel)
        endif()
    endif()
    target_compile_definitions(symbolengine_host_tests PRIVATE
        "SYMBOLENGINE_SCAN_WORKER=\"$<TARGET_FILE:symbolengine_host_scan_worker>\""
        "SYMBOLENGINE_MISBEHAVING=\"$<TARGET_PROPERTY:symbolengine_misbehaving_vst3,SYMBOLENGINE_FOLDER>\""
        "SYMBOLENGINE_TEST_VST3=\"$<TARGET_GENEX_EVAL:symbolengine_test_vst3,$<TARGET_PROPERTY:symbolengine_test_vst3,SYMBOLENGINE_BUNDLE>>\""
        "SYMBOLENGINE_TEST_VST3_LOG=\"${CMAKE_CURRENT_BINARY_DIR}/test-vst3-log.txt\""
        "SYMBOLENGINE_MODULE_LIFE=\"$<TARGET_FILE:symbolengine_host_module_life>\"")
    add_dependencies(symbolengine_host_tests symbolengine_test_vst3 symbolengine_host_module_life
                     symbolengine_host_scan_worker symbolengine_misbehaving_vst3)
endif()
