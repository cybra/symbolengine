add_executable(PlayTone PlayTone.cpp)
target_link_libraries(PlayTone PRIVATE SymbolEngine::devices)
symbolengine_set_warnings(PlayTone)

# Only its listing, which opens nothing and passes with no devices at all. --play makes a sound, so a person runs that.
# The line it looks for is the program's last, so a crash or a hang on the way there fails the test.
if(SYMBOLENGINE_TESTS)
    add_test(NAME "PlayTone: lists the devices" COMMAND PlayTone)
    set_tests_properties("PlayTone: lists the devices" PROPERTIES PASS_REGULAR_EXPRESSION "Devices found: [0-9]+")
endif()
