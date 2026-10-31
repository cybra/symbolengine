add_executable(PluginPlayer PluginPlayer.cpp)
target_link_libraries(PluginPlayer PRIVATE SymbolEngine::host)
symbolengine_set_warnings(PluginPlayer)

# What lets it watch for unsafe calls on a Mac and Linux, which has to come with it as it starts. Windows needs nothing.
if(TARGET SymbolEngine::host_unsafe_calls)
    target_link_libraries(PluginPlayer PRIVATE SymbolEngine::host_unsafe_calls)
endif()

# It plays the Gain example, as what a guide shows has to go on working.
if(SYMBOLENGINE_TESTS AND TARGET Gain_vst3)
    if(APPLE)
        set(gain $<TARGET_BUNDLE_DIR:Gain_vst3>)
    else()
        set(gain $<PATH:GET_PARENT_PATH,$<PATH:GET_PARENT_PATH,$<PATH:GET_PARENT_PATH,$<TARGET_FILE:Gain_vst3>>>>)
    endif()
    add_test(NAME "PluginPlayer: plays the Gain example" COMMAND PluginPlayer ${gain})
    set_tests_properties("PluginPlayer: plays the Gain example" PROPERTIES PASS_REGULAR_EXPRESSION "500 blocks")
endif()
