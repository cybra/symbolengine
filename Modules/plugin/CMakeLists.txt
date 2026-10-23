include(SymbolEnginePlugins)

# Plugins are built with the CMake functions above, so this module is taken in with FetchContent, not find_package.
symbolengine_add_module(plugin NOT_INSTALLED
    SOURCES
        src/ClapAdapter.cpp
        src/Parameters.cpp
        src/Plugin.cpp
    DEPENDS core)

target_link_libraries(symbolengine_plugin PUBLIC clap PRIVATE clap-wrapper-extensions)
target_include_directories(symbolengine_plugin PRIVATE src)

if(SYMBOLENGINE_TESTS)
    symbolengine_add_tests(plugin SOURCES
        Tests/ClapTests.cpp
        Tests/ParameterTests.cpp)
    target_include_directories(symbolengine_plugin_tests PRIVATE src)
    target_link_libraries(symbolengine_plugin_tests PRIVATE clap-wrapper-extensions)
    include(SymbolEngineValidation)
endif()
