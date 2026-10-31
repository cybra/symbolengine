symbolengine_add_plugin(Filter
    SOURCES Filter.cpp
    OUTPUT_NAME "SymbolEngine Filter"
    BUNDLE_IDENTIFIER dev.symbolengine.examples.filter
    FORMATS CLAP VST3 AUV2
    AU_SUBTYPE Filt
    AU_MANUFACTURER_CODE SyEn
    AU_MANUFACTURER_NAME SymbolEngine
    AU_NAME Filter)

symbolengine_add_files(Filter_impl FOLDER web FUNCTION filterEditorFiles)
symbolengine_set_warnings(Filter_impl)

if(SYMBOLENGINE_TESTS)
    symbolengine_validate_plugin(Filter)
    if(TARGET symbolengine_clap_two_copies AND TARGET symbolengine_ui AND SYMBOLENGINE_HOSTS_CAN_LOAD_PLUGINS)
        add_test(NAME "Filter: two copies in one process" COMMAND symbolengine_clap_two_copies $<TARGET_FILE:Filter_clap>)
        set_tests_properties("Filter: two copies in one process" PROPERTIES RUN_SERIAL TRUE)
    endif()
endif()
