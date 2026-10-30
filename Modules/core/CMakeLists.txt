include(FetchContent)

# miniz, for gzip, pinned by checksum, under the MIT license whose notice a product includes with its others. Its own
# build isn't run; its compressing and decompressing sources are compiled here, left out of the module's public headers.
FetchContent_Declare(symbolengine_miniz
    URL https://github.com/richgel999/miniz/archive/refs/tags/3.0.2.tar.gz
    URL_HASH SHA256=c4b4c25a4eb81883448ff8924e6dba95c800094a198dc9ce66a292ac2ef8e018
    SOURCE_SUBDIR fetched-only)
FetchContent_MakeAvailable(symbolengine_miniz)

set(miniz_generated ${CMAKE_CURRENT_BINARY_DIR}/miniz)
file(MAKE_DIRECTORY ${miniz_generated})
file(WRITE ${miniz_generated}/miniz_export.h "#define MINIZ_EXPORT\n")
add_library(symbolengine_miniz OBJECT
    ${symbolengine_miniz_SOURCE_DIR}/miniz.c
    ${symbolengine_miniz_SOURCE_DIR}/miniz_tdef.c
    ${symbolengine_miniz_SOURCE_DIR}/miniz_tinfl.c)
target_include_directories(symbolengine_miniz PUBLIC ${symbolengine_miniz_SOURCE_DIR} ${miniz_generated})
target_compile_definitions(symbolengine_miniz PUBLIC
    MINIZ_NO_ARCHIVE_APIS MINIZ_NO_STDIO MINIZ_NO_TIME MINIZ_NO_ZLIB_COMPATIBLE_NAMES)
set_target_properties(symbolengine_miniz PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
if(MSVC)
    target_compile_options(symbolengine_miniz PRIVATE /w)
else()
    target_compile_options(symbolengine_miniz PRIVATE -w)
endif()

set(sources
    src/ChannelLayout.cpp
    src/ChildProcess.cpp
    src/Files.cpp
    src/Gzip.cpp
    $<TARGET_OBJECTS:symbolengine_miniz>
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
    list(APPEND sources src/Paths_mac.cpp src/ChildProcess_posix.cpp src/MappedFile_posix.cpp)
elseif(WIN32)
    list(APPEND sources src/Paths_windows.cpp src/ChildProcess_windows.cpp src/MappedFile_windows.cpp)
    list(APPEND libraries shell32 ole32)
else()
    list(APPEND sources src/Paths_linux.cpp src/ChildProcess_posix.cpp src/MappedFile_posix.cpp)
endif()

symbolengine_add_module(core SOURCES ${sources} LIBRARIES ${libraries})
target_include_directories(symbolengine_core PRIVATE ${symbolengine_miniz_SOURCE_DIR} ${miniz_generated})
target_compile_definitions(symbolengine_core PRIVATE
    MINIZ_NO_ARCHIVE_APIS MINIZ_NO_STDIO MINIZ_NO_TIME MINIZ_NO_ZLIB_COMPATIBLE_NAMES)

if(SYMBOLENGINE_TESTS)
    # A program the child process tests start, doing what its first argument names.
    add_executable(symbolengine_core_child_helper Tests/ChildHelper.cpp)
    target_link_libraries(symbolengine_core_child_helper PRIVATE SymbolEngine::core)
    symbolengine_set_warnings(symbolengine_core_child_helper)

    symbolengine_add_tests(core ALONE "[timing]" SOURCES
        Tests/ChannelLayoutTests.cpp
        Tests/ChildProcessTests.cpp
        Tests/FilesTests.cpp
        Tests/GzipTests.cpp
        Tests/JsonTests.cpp
        Tests/LockFreeFifoTests.cpp
        Tests/MappedFileTests.cpp
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
