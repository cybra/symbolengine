cmake_minimum_required(VERSION 3.25)

project(SymbolEngine
    VERSION 0.1.0
    DESCRIPTION "An open-source C++ framework for audio applications and plugins"
    HOMEPAGE_URL "https://symbolengine.dev"
    LANGUAGES C CXX)

option(SYMBOLENGINE_TESTS "Build SymbolEngine's tests" ${PROJECT_IS_TOP_LEVEL})
option(SYMBOLENGINE_INSTALL "Add SymbolEngine's install rules, for find_package" ${PROJECT_IS_TOP_LEVEL})
option(SYMBOLENGINE_UI "Build the ui module, which fetches CHOC for the system's web view" ON)
option(SYMBOLENGINE_PLUGIN "Build the plugin module, which fetches the CLAP SDK and clap-wrapper" ON)
option(SYMBOLENGINE_EXAMPLES "Build SymbolEngine's examples" ${PROJECT_IS_TOP_LEVEL})
option(SYMBOLENGINE_WARNINGS_AS_ERRORS "Treat warnings in SymbolEngine's own code as errors" OFF)

if(APPLE AND CMAKE_OSX_DEPLOYMENT_TARGET AND CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS 11.0)
    message(FATAL_ERROR "SymbolEngine needs macOS 11.0 or later; CMAKE_OSX_DEPLOYMENT_TARGET is ${CMAKE_OSX_DEPLOYMENT_TARGET}")
endif()

if(APPLE)
    enable_language(OBJC OBJCXX)
endif()

# A plugin is best linked with the C runtime inside it, so it never depends on the version a computer has installed.
# A project taking SymbolEngine in chooses for itself; this is the choice for SymbolEngine's own builds.
if(PROJECT_IS_TOP_LEVEL AND NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(SymbolEngineModule)

if(SYMBOLENGINE_TESTS)
    enable_testing()
    include(SymbolEngineTesting)
endif()

add_subdirectory(Modules/core)
add_subdirectory(Modules/files)
if(SYMBOLENGINE_UI)
    add_subdirectory(Modules/ui)
    add_subdirectory(Modules/app)
    include(SymbolEngineApps)
endif()
if(SYMBOLENGINE_PLUGIN)
    add_subdirectory(Modules/plugin)
endif()

if(SYMBOLENGINE_INSTALL)
    include(SymbolEngineInstall)
endif()

if(SYMBOLENGINE_EXAMPLES)
    add_subdirectory(Examples)
endif()

if(SYMBOLENGINE_TESTS)
    add_subdirectory(Tests)
endif()
