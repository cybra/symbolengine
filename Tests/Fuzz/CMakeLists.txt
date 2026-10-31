# Everything SymbolEngine reads from outside, each a target a fuzzer calls with bytes. With SYMBOLENGINE_FUZZ, in a build
# whose flags hold -fsanitize=fuzzer-no-link, each is a libFuzzer program, run by run.py. Otherwise each is a program
# running once over what is kept in Seeds, so whatever a fuzzer found stays a test on every system.

set(fuzz_targets "")

function(symbolengine_add_fuzz name)
    cmake_parse_arguments(PARSE_ARGV 1 fuzz "" "" "SOURCES;LIBRARIES;SEEDS")
    set(target symbolengine_fuzz_${name})

    if(SYMBOLENGINE_FUZZ)
        add_executable(${target} ${fuzz_SOURCES})
        target_link_options(${target} PRIVATE -fsanitize=fuzzer)
    else()
        add_executable(${target} ${fuzz_SOURCES} Replay.cpp)
    endif()

    target_link_libraries(${target} PRIVATE ${fuzz_LIBRARIES})
    symbolengine_set_warnings(${target})

    set(seeds ${CMAKE_CURRENT_SOURCE_DIR}/Seeds/${name} ${fuzz_SEEDS})
    set_target_properties(${target} PROPERTIES SYMBOLENGINE_FUZZ_SEEDS "${seeds}")

    if(SYMBOLENGINE_TESTS)
        add_test(NAME "fuzz seeds: ${name}" COMMAND ${target} -runs=0 ${seeds})
    endif()

    set(fuzz_targets ${fuzz_targets} ${target} PARENT_SCOPE)
endfunction()

symbolengine_add_fuzz(audio_files SOURCES AudioFiles.cpp LIBRARIES SymbolEngine::files
    SEEDS ${PROJECT_SOURCE_DIR}/Modules/files/Tests/Data)
symbolengine_add_fuzz(text SOURCES Text.cpp LIBRARIES SymbolEngine::core)
symbolengine_add_fuzz(gzip SOURCES Gzip.cpp LIBRARIES SymbolEngine::core)
symbolengine_add_fuzz(plugin_files SOURCES PluginFiles.cpp LIBRARIES SymbolEngine::host)
symbolengine_add_fuzz(broadcast SOURCES Broadcast.cpp LIBRARIES SymbolEngine::files)

add_custom_target(symbolengine_fuzz DEPENDS ${fuzz_targets})
