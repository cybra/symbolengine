symbolengine_add_plugin(Gain
    SOURCES Gain.cpp
    OUTPUT_NAME "SymbolEngine Gain"
    BUNDLE_IDENTIFIER dev.symbolengine.examples.gain
    FORMATS CLAP)

symbolengine_set_warnings(Gain_impl)

# CLAP's own validator, when continuous integration has fetched it.
if(SYMBOLENGINE_TESTS AND SYMBOLENGINE_CLAP_VALIDATOR)
    if(APPLE)
        set(clap_file $<TARGET_BUNDLE_DIR:Gain_clap>)
    else()
        set(clap_file $<TARGET_FILE:Gain_clap>)
    endif()
    add_test(NAME "Gain: clap-validator" COMMAND ${SYMBOLENGINE_CLAP_VALIDATOR} validate ${clap_file})
endif()
