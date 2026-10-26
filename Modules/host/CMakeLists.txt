# The host's watcher thread and the tests' audio threads.
find_package(Threads REQUIRED)

set(sources src/FakePluginFormat.cpp src/Plugin.cpp src/ProcessState.cpp)
set(libraries Threads::Threads)

if(APPLE)
    list(APPEND sources src/ProcessState_mac.mm)
    list(APPEND libraries "-framework AppKit")
endif()

symbolengine_add_module(host SOURCES ${sources} DEPENDS core LIBRARIES ${libraries})

if(SYMBOLENGINE_TESTS)
    symbolengine_add_tests(host SOURCES Tests/PluginLifeTests.cpp)
endif()
