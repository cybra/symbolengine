if(SYMBOLENGINE_PLUGIN)
    add_subdirectory(Gain)
    add_subdirectory(Filter)
endif()

if(TARGET symbolengine_app)
    add_subdirectory(Hello)
endif()

add_subdirectory(PluginPlayer)
add_subdirectory(AudioConvert)
add_subdirectory(PlayTone)
add_subdirectory(Fetch)
