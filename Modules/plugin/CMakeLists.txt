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

# Editors are web pages, so they come with the ui module's web view, on Mac and Windows.
include(SymbolEngineResources)
if(TARGET symbolengine_ui)
    target_sources(symbolengine_plugin PRIVATE src/Editor.cpp)
    target_link_libraries(symbolengine_plugin PRIVATE symbolengine_ui)
    target_compile_definitions(symbolengine_plugin PRIVATE SYMBOLENGINE_EDITORS=1)
    symbolengine_add_files(symbolengine_plugin FOLDER web FUNCTION symbolengine_plugin_page_files)
endif()

if(SYMBOLENGINE_TESTS)
    # The editor's tests open a web view, so they run alone, where the system has one.
    if(TARGET symbolengine_ui)
        set(alone ALONE "[webview]")
    endif()
    symbolengine_add_tests(plugin SOURCES
        Tests/ClapTests.cpp
        Tests/ParameterTests.cpp
        ${alone})
    target_include_directories(symbolengine_plugin_tests PRIVATE src)
    target_link_libraries(symbolengine_plugin_tests PRIVATE clap-wrapper-extensions)
    symbolengine_add_files(symbolengine_plugin_tests FOLDER Tests/EditorPage FUNCTION testEditorFiles)
    if(TARGET symbolengine_ui)
        FetchContent_GetProperties(symbolengine_choc)
        target_sources(symbolengine_plugin_tests PRIVATE Tests/EditorTests.cpp)
        target_link_libraries(symbolengine_plugin_tests PRIVATE symbolengine_ui)
        target_include_directories(symbolengine_plugin_tests SYSTEM PRIVATE ${symbolengine_choc_SOURCE_DIR})
    endif()
endif()

# symbolengine_validate_plugin, which a project can have for its own plugins without building SymbolEngine's tests.
option(SYMBOLENGINE_PLUGIN_VALIDATION "Make symbolengine_validate_plugin available" ${SYMBOLENGINE_TESTS})
if(SYMBOLENGINE_PLUGIN_VALIDATION)
    include(SymbolEngineValidation)
endif()
