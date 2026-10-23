cmake_minimum_required(VERSION 3.25)

project(Consumer LANGUAGES CXX)

if(CONSUME STREQUAL "fetch")
    include(FetchContent)
    FetchContent_Declare(SymbolEngine SOURCE_DIR ${SYMBOLENGINE_SOURCE})
    FetchContent_MakeAvailable(SymbolEngine)
else()
    find_package(SymbolEngine 0.1 REQUIRED COMPONENTS core)
endif()

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE SymbolEngine::core)
