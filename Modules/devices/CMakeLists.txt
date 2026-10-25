# The control threads and the fake's threads.
find_package(Threads REQUIRED)

symbolengine_add_module(devices
    SOURCES
        src/AudioDevices.cpp
        src/AudioSetup.cpp
        src/AudioStream.cpp
        src/AudioThread.cpp
        src/FakeAudioSystem.cpp
    DEPENDS core
    LIBRARIES Threads::Threads)

if(SYMBOLENGINE_TESTS)
    symbolengine_add_tests(devices SOURCES
        Tests/AudioDevicesTests.cpp
        Tests/AudioSetupTests.cpp
        Tests/AudioStreamTests.cpp)
    target_link_libraries(symbolengine_devices_tests PRIVATE Threads::Threads)
endif()
