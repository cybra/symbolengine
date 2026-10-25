# The control threads and the fake's threads.
find_package(Threads REQUIRED)

# Each computer's own systems: Core Audio on a Mac, Windows Audio on Windows.
set(libraries Threads::Threads)
if(APPLE)
    set(systems src/CoreAudio_mac.cpp)
    list(APPEND libraries "-framework CoreAudio" "-framework CoreFoundation" "-framework IOKit")
elseif(WIN32)
    set(systems src/WindowsAudio_windows.cpp)
    list(APPEND libraries ole32 avrt powrprof)
else()
    set(systems src/Systems_other.cpp)
endif()

symbolengine_add_module(devices
    SOURCES
        src/AudioDevices.cpp
        src/AudioSetup.cpp
        src/AudioStream.cpp
        src/AudioThread.cpp
        src/FakeAudioSystem.cpp
        ${systems}
    DEPENDS core
    LIBRARIES ${libraries})

if(SYMBOLENGINE_TESTS)
    set(tests Tests/AudioDevicesTests.cpp Tests/AudioSetupTests.cpp Tests/AudioStreamTests.cpp)
    if(APPLE)
        list(APPEND tests Tests/CoreAudioTests.cpp)
    elseif(WIN32)
        list(APPEND tests Tests/WindowsAudioTests.cpp)
    endif()
    symbolengine_add_tests(devices SOURCES ${tests})
    target_link_libraries(symbolengine_devices_tests PRIVATE Threads::Threads)
    target_include_directories(symbolengine_devices_tests PRIVATE src)
endif()
