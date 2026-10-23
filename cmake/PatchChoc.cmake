# cmake -DFILE=<choc_WebView.h> -P PatchChoc.cmake
#
# Adds a userDataFolder option to CHOC's web view, so a program or plugin chooses where WebView2 keeps its data on
# Windows instead of a folder named after the program that loaded it. Each change looks for the text it replaces and
# stops the build if CHOC has changed it, so a new version of CHOC is never patched wrongly.

file(READ "${FILE}" source)

if(source MATCHES "std::string userDataFolder;")
    return()
endif()

macro(replace_once old new)
    string(FIND "${source}" "${old}" at)
    if(at EQUAL -1)
        message(FATAL_ERROR "PatchChoc.cmake: CHOC's web view has changed; this text is gone: ${old}")
    endif()
    string(REPLACE "${old}" "${new}" source "${source}")
endmacro()

replace_once([[        std::string customSchemeURI;
]] [[        std::string customSchemeURI;

        /// Windows only: the folder WebView2 keeps its data in, as UTF-8. Leave this
        /// empty for the default, a folder named after the program in AppData.
        std::string userDataFolder;
]])

replace_once([[if (auto userDataFolder = getUserDataFolder(); ! userDataFolder.empty())]]
    [[if (auto userDataFolder = options.userDataFolder.empty() ? getUserDataFolder() : createUTF16StringFromUTF8 (options.userDataFolder); ! userDataFolder.empty())]])

file(WRITE "${FILE}" "${source}")
