include(SymbolEngineCodecs)

# M4A, AAC and CAF are read by the system's own decoders, where it has them.
set(libraries)
if(APPLE)
    set(system_audio src/SystemAudio_mac.cpp)
    list(APPEND libraries "-framework AudioToolbox" "-framework CoreFoundation")
elseif(WIN32)
    set(system_audio src/SystemAudio_windows.cpp)
    list(APPEND libraries ole32 mfuuid)
else()
    set(system_audio src/SystemAudio_other.cpp)
    list(APPEND libraries m)
endif()

symbolengine_add_module(files
    SOURCES
        src/AudioFile.cpp
        src/Aiff.cpp
        src/BroadcastWave.cpp
        src/Flac.cpp
        src/Mp3.cpp
        src/OggVorbis.cpp
        src/Opus.cpp
        src/RandomAccessFile.cpp
        src/Samples.cpp
        src/Wav.cpp
        ${system_audio}
        $<TARGET_OBJECTS:symbolengine_codecs>
    DEPENDS core
    LIBRARIES ${libraries})

# The codecs' headers, seen by the module's own sources alone, and their settings as the codecs were built with.
target_include_directories(symbolengine_files SYSTEM PRIVATE ${codec_includes})
target_compile_definitions(symbolengine_files PRIVATE FLAC__NO_DLL OPUS_EXPORT= OPE_EXPORT=)

if(SYMBOLENGINE_TESTS)
    symbolengine_add_tests(files SOURCES
        Tests/AiffTests.cpp
        Tests/AudioFileTests.cpp
        Tests/BroadcastWaveTests.cpp
        Tests/CodecTests.cpp
        Tests/Mp3Tests.cpp
        Tests/SystemAudioTests.cpp
        Tests/WavTests.cpp)

    # The reference files, made by other encoders (Tests/Data/make_reference_files.py), are read where they are.
    target_compile_definitions(symbolengine_files_tests PRIVATE
        SYMBOLENGINE_FILES_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/Tests/Data")
    target_include_directories(symbolengine_files_tests PRIVATE ${PROJECT_SOURCE_DIR}/Modules/core/Tests)
endif()
