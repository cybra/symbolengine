# Lays out a copy of a built test VST3 module under each of the names given, as each system's layout of a bundle says:
#   cmake -DBUILT=<bundle on a Mac, binary elsewhere> -DINTO=<folder> -DNAMES=<one|two|...> -DARCHITECTURE=<folder
#         in Contents, none on a Mac> -DSUFFIX=<binary's suffix> -P LayOutBundles.cmake
string(REPLACE "|" ";" names "${NAMES}")
file(REMOVE_RECURSE "${INTO}")
foreach(name IN LISTS names)
    set(bundle "${INTO}/${name}.vst3")
    if(ARCHITECTURE STREQUAL "")
        file(COPY "${BUILT}/Contents" DESTINATION "${bundle}")
    else()
        file(MAKE_DIRECTORY "${bundle}/Contents/${ARCHITECTURE}")
        file(COPY_FILE "${BUILT}" "${bundle}/Contents/${ARCHITECTURE}/${name}${SUFFIX}")
    endif()
endforeach()
