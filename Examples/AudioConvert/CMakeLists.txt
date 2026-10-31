add_executable(AudioConvert AudioConvert.cpp)
target_link_libraries(AudioConvert PRIVATE SymbolEngine::files)
symbolengine_set_warnings(AudioConvert)

# It writes a tone, converts it to every format and checks what comes back, as what a guide shows has to go on working.
if(SYMBOLENGINE_TESTS)
    add_test(NAME "AudioConvert: converts a tone to every format"
             COMMAND AudioConvert --tone ${CMAKE_CURRENT_BINARY_DIR}/tone)
endif()
