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
endif()
