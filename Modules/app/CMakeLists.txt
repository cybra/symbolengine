# A desktop program around the ui module: the system's application object, the message loop, and what the system asks
# of a running program. Built where the ui module is; Linux follows the first release.
if(NOT (APPLE OR WIN32))
    message(STATUS "SymbolEngine: the app module is built on Mac and Windows; Linux follows the first release")
    return()
endif()

if(APPLE)
    set(sources src/Application_mac.mm)
    set(libraries "-framework AppKit" "-framework Foundation")
else()
    set(sources src/Application_windows.cpp)
    set(libraries user32 shell32)
endif()

symbolengine_add_module(app
    SOURCES src/Application.cpp ${sources}
    DEPENDS core ui
    LIBRARIES ${libraries})

if(SYMBOLENGINE_TESTS)
    # A second instance of a program, which the tests start to check the hand-over between instances.
    add_executable(symbolengine_app_second_instance Tests/SecondInstance.cpp)
    target_link_libraries(symbolengine_app_second_instance PRIVATE SymbolEngine::app)
    symbolengine_set_warnings(symbolengine_app_second_instance)

    symbolengine_add_tests(app SOURCES Tests/ApplicationTests.cpp)
    target_compile_definitions(symbolengine_app_tests PRIVATE
        "SYMBOLENGINE_SECOND_INSTANCE=\"$<TARGET_FILE:symbolengine_app_second_instance>\"")
    add_dependencies(symbolengine_app_tests symbolengine_app_second_instance)
    if(APPLE)
        target_sources(symbolengine_app_tests PRIVATE Tests/ApplicationTests_mac.mm)
    endif()
endif()
