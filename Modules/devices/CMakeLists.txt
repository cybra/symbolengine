# The control threads and the fake's threads.
find_package(Threads REQUIRED)

# Each computer's own systems: Core Audio on a Mac, Windows Audio on Windows.
set(libraries Threads::Threads)
if(APPLE)
    set(systems src/AudioWorkgroup_mac.cpp src/CoreAudio_mac.cpp src/Microphone_mac.mm)
    list(APPEND libraries "-framework AVFoundation" "-framework CoreAudio" "-framework CoreFoundation" "-framework IOKit")
elseif(WIN32)
    set(systems src/AudioWorkgroup_windows.cpp src/WindowsAudio_windows.cpp src/Microphone_other.cpp)
    list(APPEND libraries ole32 avrt powrprof)

    # ASIO, built with the developer's own copy of Steinberg's ASIO SDK, which is never in this repository.
    set(SYMBOLENGINE_ASIO_SDK "" CACHE PATH "Steinberg's ASIO SDK, the folder holding common/asio.h; ASIO is built when it is given")
    if(SYMBOLENGINE_ASIO_SDK)
        if(NOT EXISTS "${SYMBOLENGINE_ASIO_SDK}/common/iasiodrv.h")
            message(FATAL_ERROR "SYMBOLENGINE_ASIO_SDK names ${SYMBOLENGINE_ASIO_SDK}, which has no common/iasiodrv.h.")
        endif()
        list(APPEND systems src/Asio_windows.cpp)
        set(asio TRUE)
    endif()
else()
    set(systems src/AudioWorkgroup_other.cpp src/Systems_other.cpp src/Microphone_other.cpp)
endif()

symbolengine_add_module(devices
    SOURCES
        src/AsioDetail.cpp
        src/AudioBridge.cpp
        src/AudioDevices.cpp
        src/AudioRecordFormat.cpp
        src/AudioPlayer.cpp
        src/AudioSetup.cpp
        src/AudioStream.cpp
        src/AudioThread.cpp
        src/FakeAudioSystem.cpp
        src/RecordingAudioSystem.cpp
        ${systems}
    DEPENDS core
    LIBRARIES ${libraries})

if(asio)
    target_include_directories(symbolengine_devices SYSTEM PRIVATE "${SYMBOLENGINE_ASIO_SDK}/common")
    target_compile_definitions(symbolengine_devices PRIVATE SYMBOLENGINE_ASIO=1)
endif()

if(SYMBOLENGINE_TESTS)
    set(tests Tests/AsioDetailTests.cpp Tests/AudioBridgeTests.cpp Tests/AudioDevicesTests.cpp Tests/AudioPlayerTests.cpp Tests/AudioSetupTests.cpp Tests/AudioStreamTests.cpp
        Tests/RecordingTests.cpp)
    if(APPLE)
        list(APPEND tests Tests/CoreAudioTests.cpp)
    elseif(WIN32)
        list(APPEND tests Tests/WindowsAudioTests.cpp)
        if(asio)
            list(APPEND tests Tests/AsioTests_windows.cpp)
        endif()
    endif()
    symbolengine_add_tests(devices SOURCES ${tests} ALONE "[timing]")
    target_link_libraries(symbolengine_devices_tests PRIVATE Threads::Threads)
    target_include_directories(symbolengine_devices_tests PRIVATE src)

    # What real hardware does while a person sleeps the computer or unplugs a device, written for the fake to replay.
    add_executable(symbolengine_devices_recorder Tests/Recorder.cpp)
    target_link_libraries(symbolengine_devices_recorder PRIVATE SymbolEngine::devices)
    target_include_directories(symbolengine_devices_recorder PRIVATE src)
    symbolengine_set_warnings(symbolengine_devices_recorder)
    if(asio)
        target_include_directories(symbolengine_devices_tests SYSTEM PRIVATE "${SYMBOLENGINE_ASIO_SDK}/common")
        target_compile_definitions(symbolengine_devices_tests PRIVATE SYMBOLENGINE_ASIO=1)
    endif()
endif()
