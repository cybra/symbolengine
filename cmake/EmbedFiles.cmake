# cmake -DFOLDER=<folder> -DFILES=<file|file|...> -DFUNCTION=<name> -DOUTPUT=<source> -P EmbedFiles.cmake
#
# Writes a C++ source holding each file's bytes, and the function that finds one by its path from the folder.

string(REPLACE "|" ";" files "${FILES}")

set(arrays "")
set(entries "")
set(index 0)

foreach(file IN LISTS files)
    file(RELATIVE_PATH path "${FOLDER}" "${file}")
    file(READ "${file}" hex HEX)
    file(SIZE "${file}" size)

    # Sixteen bytes to a line, each as 0x00.
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
    string(REGEX REPLACE "((0x[0-9a-f][0-9a-f],){16})" "\\1\n    " bytes "${bytes}")

    if(size EQUAL 0)
        string(APPEND arrays "const unsigned char file${index}[1] = {0};\n")
    else()
        string(APPEND arrays "const unsigned char file${index}[] = {\n    ${bytes}};\n")
    endif()
    string(APPEND entries "    {\"/${path}\", file${index}, ${size}},\n")
    math(EXPR index "${index} + 1")
endforeach()

file(WRITE "${OUTPUT}.new" "// Written by SymbolEngine's EmbedFiles.cmake from ${FOLDER}; changes here are lost.

#include <cstddef>
#include <optional>
#include <string_view>

namespace
{
${arrays}
struct File
{
    std::string_view path;
    const unsigned char* bytes;
    std::size_t size;
};

const File files[] = {
${entries}    {{}, nullptr, 0}};
}

std::optional<std::string_view> ${FUNCTION}(std::string_view path);

std::optional<std::string_view> ${FUNCTION}(std::string_view path)
{
    for (const auto& file : files)
        if (file.bytes != nullptr && file.path == path)
            return std::string_view(reinterpret_cast<const char*>(file.bytes), file.size);
    return std::nullopt;
}
")

# Only a change is written, so an unchanged folder doesn't rebuild what uses it.
file(COPY_FILE "${OUTPUT}.new" "${OUTPUT}" ONLY_IF_DIFFERENT)
file(REMOVE "${OUTPUT}.new")
