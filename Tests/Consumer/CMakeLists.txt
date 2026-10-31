cmake_minimum_required(VERSION 3.25)

project(Consumer VERSION 1.0.0 LANGUAGES CXX)

enable_testing()

if(CONSUME STREQUAL "fetch")
    # As the plugin guide says: an Audio Unit is built from Objective-C++ as well, and validators are asked for. Nothing
    # here asks for a standard of C++, as a project may not.
    if(APPLE)
        enable_language(OBJC OBJCXX)
    endif()

    set(SYMBOLENGINE_PLUGIN_VALIDATION ON)
    include(FetchContent)
    FetchContent_Declare(SymbolEngine SOURCE_DIR ${SYMBOLENGINE_SOURCE})
    FetchContent_MakeAvailable(SymbolEngine)
else()
    find_package(SymbolEngine 0.1 REQUIRED COMPONENTS core files devices)
endif()

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE SymbolEngine::core SymbolEngine::files SymbolEngine::devices)
add_test(NAME consumer COMMAND consumer)

if(CONSUME STREQUAL "fetch" AND TARGET SymbolEngine::plugin)
    symbolengine_add_plugin(ConsumerGain
        SOURCES ${SYMBOLENGINE_SOURCE}/Examples/Gain/Gain.cpp
        BUNDLE_IDENTIFIER test.symbolengine.consumer.gain
        FORMATS CLAP VST3 AUV2
        AU_SUBTYPE CnGn
        AU_MANUFACTURER_CODE SyTs
        AU_MANUFACTURER_NAME "SymbolEngine Tests"
        AU_NAME "Consumer Gain")
    add_dependencies(consumer ConsumerGain_all)
    symbolengine_validate_plugin(ConsumerGain)
endif()
