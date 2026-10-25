cmake_minimum_required(VERSION 3.25)

project(Consumer LANGUAGES CXX)

if(CONSUME STREQUAL "fetch")
    include(FetchContent)
    FetchContent_Declare(SymbolEngine SOURCE_DIR ${SYMBOLENGINE_SOURCE})
    FetchContent_MakeAvailable(SymbolEngine)
else()
    find_package(SymbolEngine 0.1 REQUIRED COMPONENTS core files devices)
endif()

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE SymbolEngine::core SymbolEngine::files SymbolEngine::devices)

if(CONSUME STREQUAL "fetch" AND TARGET SymbolEngine::plugin)
    symbolengine_add_plugin(ConsumerGain
        SOURCES ${SYMBOLENGINE_SOURCE}/Examples/Gain/Gain.cpp
        BUNDLE_IDENTIFIER test.symbolengine.consumer.gain)
    add_dependencies(consumer ConsumerGain_all)
endif()
