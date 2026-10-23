cmake_minimum_required(VERSION 3.25)

project(SymbolEngine
    VERSION 0.1.0
    DESCRIPTION "An open-source C++ framework for audio applications and plugins"
    HOMEPAGE_URL "https://symbolengine.dev"
    LANGUAGES CXX)

option(SYMBOLENGINE_TESTS "Build SymbolEngine's tests" ${PROJECT_IS_TOP_LEVEL})
option(SYMBOLENGINE_INSTALL "Add SymbolEngine's install rules, for find_package" ${PROJECT_IS_TOP_LEVEL})
option(SYMBOLENGINE_WARNINGS_AS_ERRORS "Treat warnings in SymbolEngine's own code as errors" OFF)

if(APPLE AND CMAKE_OSX_DEPLOYMENT_TARGET AND CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS 11.0)
    message(FATAL_ERROR "SymbolEngine needs macOS 11.0 or later; CMAKE_OSX_DEPLOYMENT_TARGET is ${CMAKE_OSX_DEPLOYMENT_TARGET}")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(SymbolEngineModule)

if(SYMBOLENGINE_TESTS)
    enable_testing()
    include(SymbolEngineTesting)
endif()

add_subdirectory(Modules/core)

if(SYMBOLENGINE_INSTALL)
    include(SymbolEngineInstall)
endif()

if(SYMBOLENGINE_TESTS)
    add_subdirectory(Tests)
endif()
