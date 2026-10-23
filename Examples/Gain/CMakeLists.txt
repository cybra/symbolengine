symbolengine_add_plugin(Gain
    SOURCES Gain.cpp
    OUTPUT_NAME "SymbolEngine Gain"
    BUNDLE_IDENTIFIER dev.symbolengine.examples.gain
    FORMATS CLAP VST3 AUV2
    AU_SUBTYPE Gain
    AU_MANUFACTURER_CODE SyEn
    AU_MANUFACTURER_NAME SymbolEngine
    AU_NAME Gain)

symbolengine_set_warnings(Gain_impl)

if(SYMBOLENGINE_TESTS)
    symbolengine_validate_plugin(Gain)
endif()
