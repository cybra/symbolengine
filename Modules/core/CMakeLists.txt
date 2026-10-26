set(sources
    src/ChannelLayout.cpp
    src/ChildProcess.cpp
    src/Files.cpp
    src/Json.cpp
    src/Numbers.cpp
    src/Paths.cpp
    src/Resampler.cpp
    src/SettingsFile.cpp
    src/Sha256.cpp
    src/System.cpp
    src/Text.cpp
    src/Utf8.cpp
    src/Value.cpp
    src/Xml.cpp)

# Child processes' links run threads of their own.
find_package(Threads REQUIRED)
set(libraries Threads::Threads)

if(APPLE)
    list(APPEND sources src/Paths_mac.cpp src/ChildProcess_posix.cpp)
elseif(WIN32)
    list(APPEND sources src/Paths_windows.cpp src/ChildProcess_windows.cpp)
    list(APPEND libraries shell32 ole32)
else()
    list(APPEND sources src/Paths_linux.cpp src/ChildProcess_posix.cpp)
endif()

symbolengine_add_module(core SOURCES ${sources} LIBRARIES ${libraries})

if(SYMBOLENGINE_TESTS)
    # A program the child process tests start, doing what its first argument names.
    add_executable(symbolengine_core_child_helper Tests/ChildHelper.cpp)
    target_link_libraries(symbolengine_core_child_helper PRIVATE SymbolEngine::core)
    symbolengine_set_warnings(symbolengine_core_child_helper)

    symbolengine_add_tests(core ALONE "[timing]" SOURCES
        Tests/ChannelLayoutTests.cpp
        Tests/ChildProcessTests.cpp
        Tests/FilesTests.cpp
        Tests/JsonTests.cpp
        Tests/LockFreeFifoTests.cpp
        Tests/NoDenormalsTests.cpp
        Tests/NumberTests.cpp
        Tests/PathsTests.cpp
        Tests/ResamplerTests.cpp
        Tests/SettingsFileTests.cpp
        Tests/Sha256Tests.cpp
        Tests/SystemTests.cpp
        Tests/TextTests.cpp
        Tests/ToCharsReference.cpp
        Tests/ValueTests.cpp
        Tests/XmlTests.cpp)

    target_compile_definitions(symbolengine_core_tests PRIVATE
        "SYMBOLENGINE_CHILD_HELPER=\"$<TARGET_FILE:symbolengine_core_child_helper>\"")
    add_dependencies(symbolengine_core_tests symbolengine_core_child_helper)

    # The number tests check the module's own way of printing a double against std::to_chars, which Apple provides
    # for doubles from macOS 13.3, so the file holding the reference is built for that.
    target_include_directories(symbolengine_core_tests PRIVATE src)
    if(APPLE)
        set_source_files_properties(Tests/ToCharsReference.cpp PROPERTIES COMPILE_OPTIONS -mmacosx-version-min=13.3)
    endif()
endif()
