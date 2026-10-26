include_guard(GLOBAL)

include(FetchContent)

# The VST3 SDK, at the version clap-wrapper is tested with, fetched the first time a plugin is built as a VST3 or the
# host module is built. Its archive leaves its parts empty, so each is fetched into its place. A developer's own copy
# can be used instead by setting VST3_SDK_ROOT.
function(symbolengine_fetch_vst3_sdk)
    if(SYMBOLENGINE_VST3_SDK_ROOT)
        return()
    endif()
    if(VST3_SDK_ROOT)
        set(SYMBOLENGINE_VST3_SDK_ROOT ${VST3_SDK_ROOT} CACHE INTERNAL "")
        return()
    endif()

    FetchContent_Declare(symbolengine_vst3sdk
        URL https://github.com/steinbergmedia/vst3sdk/archive/9fad9770f2ae8542ab1a548a68c1ad1ac690abe0.tar.gz # v3.8.0_build_66
        URL_HASH SHA256=121e1063962aea6e02817fec6bd3066162e22f262904c00b4be4e8a1fb3f826b
        SOURCE_SUBDIR fetched-only)
    FetchContent_MakeAvailable(symbolengine_vst3sdk)

    set(parts
        base 3d2e82f8e6bff59c1d8b7a27491a29c2286b5206 8f1d5f9ac0cd1e916ca3a196f5ee080ec4301b0b10a7ee7e564013f5d58b03cd
        cmake de6e54eeaaab35b7145f5c32c279b5e892146e04 ab4ce274563570fed11f2f0a31feb93280cd26543703f90c404a8720c1a486a5
        pluginterfaces 31d6eeba6daaa3e2a8bfbe3e7a90ca0b7fbfbc1c 7c9d19af0e81711edde34c3eb2e5e6d150ae1f449501bb5b352605b9590654d7
        public_sdk a3911a4615dabbfdfd9d181ee26b05c70c289a95 4cc8a9a57a970172b1efbef62025ae4f0656e132eed994f0e15ed19dfd9b0bb3)

    while(parts)
        list(POP_FRONT parts part commit hash)
        string(REPLACE "_" "." folder ${part})
        FetchContent_Declare(symbolengine_vst3_${part}
            URL https://github.com/steinbergmedia/vst3_${part}/archive/${commit}.tar.gz
            URL_HASH SHA256=${hash}
            SOURCE_DIR ${symbolengine_vst3sdk_SOURCE_DIR}/${folder}
            SOURCE_SUBDIR fetched-only)
        FetchContent_MakeAvailable(symbolengine_vst3_${part})
    endwhile()

    set(SYMBOLENGINE_VST3_SDK_ROOT ${symbolengine_vst3sdk_SOURCE_DIR} CACHE INTERNAL "")
endfunction()
