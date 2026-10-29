include(FetchContent)

# Apple's AudioUnitSDK, at the version clap-wrapper is tested with, fetched the first time a plugin is built as an
# Audio Unit or the host module's tests build their own Audio Units. A developer's own copy can be used instead by
# setting AUDIOUNIT_SDK_ROOT. The VST3 SDK's is in SymbolEngineVst3Sdk, which the host module uses too.
function(symbolengine_fetch_audiounit_sdk)
    if(SYMBOLENGINE_AUDIOUNIT_SDK_ROOT)
        return()
    endif()
    if(AUDIOUNIT_SDK_ROOT)
        set(SYMBOLENGINE_AUDIOUNIT_SDK_ROOT ${AUDIOUNIT_SDK_ROOT} CACHE INTERNAL "")
        return()
    endif()

    FetchContent_Declare(symbolengine_audiounit_sdk
        URL https://github.com/apple/AudioUnitSDK/archive/dca901da743f491cf25c0ec2503b1373c7f9f69c.tar.gz # AudioUnitSDK-1.1.0
        URL_HASH SHA256=602aba0094bf179cf587eae4b8c585c2cfa996ff1a33a9f8522e14414272a890
        SOURCE_SUBDIR fetched-only)
    FetchContent_MakeAvailable(symbolengine_audiounit_sdk)

    set(SYMBOLENGINE_AUDIOUNIT_SDK_ROOT ${symbolengine_audiounit_sdk_SOURCE_DIR} CACHE INTERNAL "")
endfunction()
