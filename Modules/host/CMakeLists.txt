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
    src/PluginCrashNote.cpp
    src/Guard.cpp
    src/Plugin.cpp
    src/PluginFiles.cpp
    src/PluginFormat.cpp
    src/PluginList.cpp
    src/PluginScanWorker.cpp
    src/PluginScanner.cpp
    src/ProcessState.cpp
    src/UnsafeCalls.cpp
    src/Vst3Format.cpp
    src/Vst3Module.cpp
    ${sdk_sources})
set(libraries Threads::Threads)

if(APPLE)
    list(APPEND sources src/AudioUnitFormat.mm src/MainThread_other.cpp src/PluginCrashNote_posix.cpp
                        src/ProcessState_mac.mm src/ScanWorkerCrash_posix.cpp src/ScanWorkerLoop_mac.mm
                        src/SystemCrashReports.cpp src/Vst3Module_mac.cpp src/Vst3View_mac.mm)
    list(APPEND libraries "-framework AppKit" "-framework AudioToolbox" "-framework CoreAudio" "-framework CoreFoundation"
                          "-framework CoreGraphics" "-framework Security")
elseif(WIN32)
    list(APPEND sources src/MainThread_windows.cpp src/PluginCrashNote_windows.cpp src/ScanWorkerCrash_windows.cpp
                        src/ScanWorkerLoop_windows.cpp src/UnsafeCalls_windows.cpp src/Vst3Module_windows.cpp
                        src/Vst3View_windows.cpp)
    list(APPEND libraries ole32 user32 version)
else()
    list(APPEND sources src/MainThread_other.cpp src/PluginCrashNote_posix.cpp src/ScanWorkerCrash_posix.cpp
                        src/ScanWorkerLoop_other.cpp src/Vst3Module_linux.cpp src/Vst3View_other.cpp)
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

# What a program links, on a Mac and Linux, to watch for unsafe calls in the plugins it hosts: a library of its own, as
# the system lets it see those calls only if it comes with the program as it starts. Never built with the sanitizers,
# which take the same calls themselves.
set(sanitized FALSE)
if(CMAKE_CXX_FLAGS MATCHES "-fsanitize")
    set(sanitized TRUE)
endif()
if(NOT WIN32 AND NOT sanitized)
    add_library(symbolengine_host_unsafe_calls SHARED src/UnsafeCallsLibrary.cpp)
    add_library(SymbolEngine::host_unsafe_calls ALIAS symbolengine_host_unsafe_calls)
    set_target_properties(symbolengine_host_unsafe_calls PROPERTIES
        EXPORT_NAME host_unsafe_calls CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
    target_compile_features(symbolengine_host_unsafe_calls PRIVATE cxx_std_20)
    target_compile_options(symbolengine_host_unsafe_calls PRIVATE -fno-builtin)
    target_link_libraries(symbolengine_host_unsafe_calls PRIVATE ${CMAKE_DL_LIBS} $<$<BOOL:${APPLE}>:objc>)
    # Nothing in a program refers to it by name, which a linker would otherwise take as a reason to leave it out.
    if(NOT APPLE)
        target_link_options(symbolengine_host_unsafe_calls INTERFACE -Wl,--no-as-needed)
    endif()
    symbolengine_set_warnings(symbolengine_host_unsafe_calls)
endif()

if(SYMBOLENGINE_TESTS)
    add_subdirectory(Tests/Vst3)

    # The program the crash note tests start, which ends in the middle of a call into a test plugin, or cleanly.
    add_executable(symbolengine_host_crash_note Tests/CrashNote.cpp)
    target_link_libraries(symbolengine_host_crash_note PRIVATE SymbolEngine::host)
    symbolengine_set_warnings(symbolengine_host_crash_note)

    # The program the scan worker tests start as a worker, and as a program with a worker of its own.
    add_executable(symbolengine_host_scan_worker Tests/ScanWorker.cpp)
    target_link_libraries(symbolengine_host_scan_worker PRIVATE SymbolEngine::host)
    if(APPLE)
        target_link_libraries(symbolengine_host_scan_worker PRIVATE "-framework AudioToolbox"
                                                                    "-framework CoreFoundation")
    endif()
    symbolengine_set_warnings(symbolengine_host_scan_worker)

    set(test_sources Tests/Allocations.cpp Tests/CrashNoteTests.cpp Tests/EditorTests.cpp Tests/FreePluginTests.cpp
                     Tests/UnsafeCallsAbsentTests.cpp
                     Tests/HostCheckerTests.cpp Tests/LateBlockTests.cpp
                     Tests/ParameterTests.cpp Tests/PluginLifeTests.cpp Tests/PluginListTests.cpp
                     Tests/PluginTimingTests.cpp Tests/ScanTests.cpp Tests/ScanWorkerTests.cpp Tests/TimeTests.cpp
                     Tests/Vst3Tests.cpp Tests/WalkTests.cpp)
    # It tells one place in the code from another by where each call is made from, which optimizing moves.
    if(NOT MSVC)
        set_source_files_properties(Tests/UnsafeCallsAbsentTests.cpp PROPERTIES COMPILE_OPTIONS "-O0")
    endif()
    if(APPLE)
        add_subdirectory(Tests/AudioUnits)
        list(APPEND test_sources Tests/AudioUnitTests.cpp Tests/ScanWorkerTests_mac.mm Tests/SignatureTests.cpp)

        # A library built for both of a Mac's architectures, which the signature tests sign and damage, without the
        # sanitizers, as nothing loads it.
        add_library(symbolengine_signed_library SHARED Tests/SignedLibrary.cpp)
        set_target_properties(symbolengine_signed_library PROPERTIES OSX_ARCHITECTURES "arm64;x86_64")
        target_compile_options(symbolengine_signed_library PRIVATE -fno-sanitize=all)
        target_link_options(symbolengine_signed_library PRIVATE -fno-sanitize=all)
    endif()
    if(TARGET symbolengine_ui)
        list(APPEND test_sources $<IF:$<BOOL:${APPLE}>,Tests/EditorTests_mac.mm,Tests/EditorTests_windows.cpp>)
    endif()
    symbolengine_add_tests(host SOURCES ${test_sources} ALONE "[timing],[windows],[free]")
    # The devices module's fake system and its tests' helpers, for a device's late blocks named by their plugin.
    target_link_libraries(symbolengine_host_tests PRIVATE SymbolEngine::devices)
    target_include_directories(symbolengine_host_tests PRIVATE src)
    target_include_directories(symbolengine_host_tests PRIVATE ${PROJECT_SOURCE_DIR}/Modules/core/Tests
                                                               ${PROJECT_SOURCE_DIR}/Modules/devices/Tests)
    if(TARGET symbolengine_ui)
        target_compile_definitions(symbolengine_host_tests PRIVATE SYMBOLENGINE_PLUGIN_WINDOWS=1)
    endif()
    if(APPLE)
        target_link_libraries(symbolengine_host_tests PRIVATE "-framework AppKit" "-framework AudioToolbox")
        target_compile_definitions(symbolengine_host_tests PRIVATE
            "SYMBOLENGINE_TEST_AUDIO_UNITS=\"$<TARGET_BUNDLE_DIR:symbolengine_test_audio_units>\""
            "SYMBOLENGINE_TEST_TWIN_AUDIO_UNITS=\"$<TARGET_BUNDLE_DIR:symbolengine_test_audio_units_twin>\""
            "SYMBOLENGINE_TEST_RAW_AUDIO_UNITS=\"$<TARGET_BUNDLE_DIR:symbolengine_test_audio_units_raw>\""
            "SYMBOLENGINE_TEST_AU_LOG=\"${CMAKE_CURRENT_BINARY_DIR}/test-au-log.txt\""
            "SYMBOLENGINE_SIGNED_LIBRARY=\"$<TARGET_FILE:symbolengine_signed_library>\"")
        add_dependencies(symbolengine_host_tests symbolengine_test_audio_units symbolengine_test_audio_units_twin
                         symbolengine_test_audio_units_raw
                         symbolengine_signed_library)
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
        "SYMBOLENGINE_HOST_CHECKER=\"$<TARGET_GENEX_EVAL:symbolengine_host_checker,$<TARGET_PROPERTY:symbolengine_host_checker,SYMBOLENGINE_BUNDLE>>\""
        "SYMBOLENGINE_MODULE_LIFE=\"$<TARGET_FILE:symbolengine_host_module_life>\""
        "SYMBOLENGINE_CRASH_NOTE=\"$<TARGET_FILE:symbolengine_host_crash_note>\"")
    if(TARGET symbolengine_host_unsafe_calls)
        target_compile_definitions(symbolengine_host_tests PRIVATE
            "SYMBOLENGINE_UNSAFE_CALLS_LIBRARY=\"$<TARGET_FILE:symbolengine_host_unsafe_calls>\"")
        add_dependencies(symbolengine_host_tests symbolengine_host_unsafe_calls)
    endif()
    add_dependencies(symbolengine_host_tests symbolengine_test_vst3 symbolengine_host_checker
                     symbolengine_host_module_life
                     symbolengine_host_crash_note
                     symbolengine_host_scan_worker symbolengine_misbehaving_vst3)

    # A program of its own for the tests of unsafe calls, linked to what watches for them as a program would be, leaving
    # every other test free of it.
    add_executable(symbolengine_host_unsafe_call_tests Tests/UnsafeCallTests.cpp)
    target_link_libraries(symbolengine_host_unsafe_call_tests PRIVATE SymbolEngine::host Catch2::Catch2WithMain)
    if(TARGET symbolengine_host_unsafe_calls)
        target_link_libraries(symbolengine_host_unsafe_call_tests PRIVATE SymbolEngine::host_unsafe_calls)
    endif()
    symbolengine_set_warnings(symbolengine_host_unsafe_call_tests)
    target_compile_definitions(symbolengine_host_unsafe_call_tests PRIVATE
        "SYMBOLENGINE_TEST_VST3=\"$<TARGET_GENEX_EVAL:symbolengine_test_vst3,$<TARGET_PROPERTY:symbolengine_test_vst3,SYMBOLENGINE_BUNDLE>>\""
        $<$<BOOL:${sanitized}>:SYMBOLENGINE_SANITIZED=1>)
    add_dependencies(symbolengine_host_unsafe_call_tests symbolengine_test_vst3)
    if(APPLE)
        target_link_libraries(symbolengine_host_unsafe_call_tests PRIVATE "-framework AudioToolbox"
                                                                          "-framework CoreFoundation")
        target_compile_definitions(symbolengine_host_unsafe_call_tests PRIVATE
            "SYMBOLENGINE_TEST_AUDIO_UNITS=\"$<TARGET_BUNDLE_DIR:symbolengine_test_audio_units>\"")
        add_dependencies(symbolengine_host_unsafe_call_tests symbolengine_test_audio_units)
    endif()
    catch_discover_tests(symbolengine_host_unsafe_call_tests TEST_PREFIX "host: " TEST_SPEC "~[free]"
                         DISCOVERY_MODE PRE_TEST)
    catch_discover_tests(symbolengine_host_unsafe_call_tests TEST_PREFIX "host: " TEST_SPEC "[free]"
                         DISCOVERY_MODE PRE_TEST PROPERTIES RUN_SERIAL TRUE)
endif()
