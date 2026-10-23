include_guard(GLOBAL)

set(SYMBOLENGINE_EMBED_SCRIPT ${CMAKE_CURRENT_LIST_DIR}/EmbedFiles.cmake CACHE INTERNAL "")

# symbolengine_add_files(<target> FOLDER <folder> FUNCTION <name>)
#
# Builds every file in the folder, and the folders inside it, into the target, and defines the function
#
#     std::optional<std::string_view> <name>(std::string_view path);
#
# which gives a file's bytes by its path from the folder, such as "/index.html" or "/images/knob.svg", or nothing for
# a path it doesn't have. The code that calls it declares it the same way. A file changed is built in again; a file
# added or removed is found the next time the build runs.
function(symbolengine_add_files target)
    cmake_parse_arguments(PARSE_ARGV 1 embed "" "FOLDER;FUNCTION" "")

    if(NOT IS_ABSOLUTE "${embed_FOLDER}")
        set(embed_FOLDER ${CMAKE_CURRENT_SOURCE_DIR}/${embed_FOLDER})
    endif()
    if(NOT embed_FUNCTION MATCHES "^[A-Za-z_][A-Za-z0-9_]*$")
        message(FATAL_ERROR "symbolengine_add_files(${target}): FUNCTION must be a plain C++ name")
    endif()

    file(GLOB_RECURSE files CONFIGURE_DEPENDS LIST_DIRECTORIES false "${embed_FOLDER}/*")
    list(FILTER files EXCLUDE REGEX "/\\.DS_Store$")
    list(SORT files)

    set(output ${CMAKE_CURRENT_BINARY_DIR}/${target}_${embed_FUNCTION}.cpp)
    string(REPLACE ";" "|" file_list "${files}")

    add_custom_command(OUTPUT ${output}
        COMMAND ${CMAKE_COMMAND} "-DFOLDER=${embed_FOLDER}" "-DFILES=${file_list}" "-DFUNCTION=${embed_FUNCTION}"
            "-DOUTPUT=${output}" -P ${SYMBOLENGINE_EMBED_SCRIPT}
        DEPENDS ${files} ${SYMBOLENGINE_EMBED_SCRIPT}
        COMMENT "Building ${embed_FUNCTION}'s files into ${target}"
        VERBATIM)

    target_sources(${target} PRIVATE ${output})
endfunction()
