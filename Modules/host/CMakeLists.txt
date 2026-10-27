# The host's watcher thread and the tests' audio threads.
find_package(Threads REQUIRED)

# Steinberg's VST3 SDK, MIT licensed: its interfaces, and the few of its hosting classes the host uses as they are,
# its host context, messages, parameter queues and event lists. Its sources build into the module with their own
# warnings left as Steinberg has them.
include(SymbolEngineVst3Sdk)
symbolengine_fetch_vst3_sdk()
set(sdk ${SYMBOLENGINE_VST3_SDK_ROOT})
set(sdk_sources
    ${sdk}/pluginterfaces/base/conststringtable.cpp
    ${sdk}/pluginterfaces/base/coreiids.cpp
    ${sdk}/pluginterfaces/base/funknown.cpp
    ${sdk}/pluginterfaces/base/ustring.cpp
    ${sdk}/public.sdk/source/common/commonstringconvert.cpp
    ${sdk}/public.sdk/source/vst/hosting/eventlist.cpp
    ${sdk}/public.sdk/source/vst/hosting/hostclasses.cpp
    ${sdk}/public.sdk/source/vst/hosting/parameterchanges.cpp
    ${sdk}/public.sdk/source/vst/hosting/pluginterfacesupport.cpp
    ${sdk}/public.sdk/source/vst/utility/stringconvert.cpp
    ${sdk}/public.sdk/source/vst/vstinitiids.cpp)
set_source_files_properties(${sdk_sources} PROPERTIES COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/W0,-w>")

set(sources
    src/FakePluginFormat.cpp
    src/Plugin.cpp
    src/ProcessState.cpp
    src/Vst3Format.cpp
    src/Vst3Module.cpp
    ${sdk_sources})
set(libraries Threads::Threads)

if(APPLE)
    list(APPEND sources src/ProcessState_mac.mm src/Vst3Module_mac.cpp)
    list(APPEND libraries "-framework AppKit" "-framework CoreFoundation")
elseif(WIN32)
    list(APPEND sources src/Vst3Module_windows.cpp)
else()
    list(APPEND sources src/Vst3Module_linux.cpp)
    list(APPEND libraries ${CMAKE_DL_LIBS})
endif()

symbolengine_add_module(host SOURCES ${sources} DEPENDS core LIBRARIES ${libraries})
target_include_directories(symbolengine_host SYSTEM PRIVATE ${sdk})

if(SYMBOLENGINE_TESTS)
    add_subdirectory(Tests/Vst3)
    symbolengine_add_tests(host SOURCES Tests/ParameterTests.cpp Tests/PluginLifeTests.cpp Tests/Vst3Tests.cpp)
    target_compile_definitions(symbolengine_host_tests PRIVATE
        "SYMBOLENGINE_TEST_VST3=\"$<TARGET_GENEX_EVAL:symbolengine_test_vst3,$<TARGET_PROPERTY:symbolengine_test_vst3,SYMBOLENGINE_BUNDLE>>\""
        "SYMBOLENGINE_TEST_VST3_LOG=\"${CMAKE_CURRENT_BINARY_DIR}/test-vst3-log.txt\""
        "SYMBOLENGINE_MODULE_LIFE=\"$<TARGET_FILE:symbolengine_host_module_life>\"")
    add_dependencies(symbolengine_host_tests symbolengine_test_vst3 symbolengine_host_module_life)
endif()
