symbolengine_add_module(files
    SOURCES
        src/AudioFile.cpp
        src/Aiff.cpp
        src/BroadcastWave.cpp
        src/RandomAccessFile.cpp
        src/Samples.cpp
        src/Wav.cpp
    DEPENDS core)

if(SYMBOLENGINE_TESTS)
    symbolengine_add_tests(files SOURCES
        Tests/AiffTests.cpp
        Tests/AudioFileTests.cpp
        Tests/BroadcastWaveTests.cpp
        Tests/WavTests.cpp)

    # The reference files, made by other encoders (Tests/Data/make_reference_files.py), are read where they are.
    target_compile_definitions(symbolengine_files_tests PRIVATE
        SYMBOLENGINE_FILES_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/Tests/Data")
    target_include_directories(symbolengine_files_tests PRIVATE ${PROJECT_SOURCE_DIR}/Modules/core/Tests)
endif()
