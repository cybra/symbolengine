set(sources
    src/ChannelLayout.cpp
    src/Files.cpp
    src/Json.cpp
    src/Numbers.cpp
    src/Paths.cpp
    src/SettingsFile.cpp
    src/System.cpp
    src/Text.cpp
    src/Utf8.cpp
    src/Value.cpp
    src/Xml.cpp)

set(libraries)

if(APPLE)
    list(APPEND sources src/Paths_mac.cpp)
elseif(WIN32)
    list(APPEND sources src/Paths_windows.cpp)
    list(APPEND libraries shell32 ole32)
else()
    list(APPEND sources src/Paths_linux.cpp)
endif()

symbolengine_add_module(core SOURCES ${sources} LIBRARIES ${libraries})

if(SYMBOLENGINE_TESTS)
    symbolengine_add_tests(core SOURCES
        Tests/ChannelLayoutTests.cpp
        Tests/FilesTests.cpp
        Tests/JsonTests.cpp
        Tests/NumberTests.cpp
        Tests/PathsTests.cpp
        Tests/SettingsFileTests.cpp
        Tests/SystemTests.cpp
        Tests/TextTests.cpp
        Tests/ToCharsReference.cpp
        Tests/ValueTests.cpp
        Tests/XmlTests.cpp)

    # The number tests check the module's own way of printing a double against std::to_chars, which Apple provides
    # for doubles from macOS 13.3, so the file holding the reference is built for that.
    target_include_directories(symbolengine_core_tests PRIVATE src)
    if(APPLE)
        set_source_files_properties(Tests/ToCharsReference.cpp PROPERTIES COMPILE_OPTIONS -mmacosx-version-min=13.3)
    endif()
endif()
