# cmake -DFOLDER=<folder> -DFILES=<file|file|...> -DFUNCTION=<name> -DOUTPUT=<source> -P EmbedFiles.cmake
#
# Writes a C++ source holding each file's bytes, and the function that finds one by its path from the folder.

string(REPLACE "|" ";" files "${FILES}")

set(ARRAYS "")
set(ENTRIES "")
set(index 0)

foreach(file IN LISTS files)
    file(RELATIVE_PATH path "${FOLDER}" "${file}")
    file(READ "${file}" hex HEX)
    file(SIZE "${file}" size)

    # Sixteen bytes to a line, each as 0x00.
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
    string(REGEX REPLACE "((0x[0-9a-f][0-9a-f],){16})" "\\1\n    " bytes "${bytes}")

    if(size EQUAL 0)
        string(APPEND ARRAYS "const unsigned char file${index}[1] = {0};\n")
    else()
        string(APPEND ARRAYS "const unsigned char file${index}[] = {\n    ${bytes}};\n")
    endif()
    string(APPEND ENTRIES "    {\"/${path}\", file${index}, ${size}},\n")
    math(EXPR index "${index} + 1")
endforeach()

set(template [=[// Written by SymbolEngine's EmbedFiles.cmake from @FOLDER@; changes here are lost.

#include <cstddef>
#include <optional>
#include <string_view>

#ifndef NDEBUG
    #include <cstdlib>
    #include <fstream>
    #include <iterator>
    #include <string>
#endif

namespace
{
@ARRAYS@
struct File
{
    std::string_view path;
    const unsigned char* bytes;
    std::size_t size;
};

const File files[] = {
@ENTRIES@    {{}, nullptr, 0}};
}

std::optional<std::string_view> @FUNCTION@(std::string_view path);

std::optional<std::string_view> @FUNCTION@(std::string_view path)
{
#ifndef NDEBUG
    // In a debug build, SYMBOLENGINE_LIVE_FILES=1 reads each file from the folder it was built in from, so a page can
    // be changed and reloaded without building again. What it gives lasts until the next call on the same thread.
    #if defined(_MSC_VER)
        #pragma warning(suppress : 4996)
    #endif
    const char* live = std::getenv("SYMBOLENGINE_LIVE_FILES");
    if (live != nullptr && live[0] == '1' && path.find("..") == std::string_view::npos)
    {
        std::ifstream in(std::string(R"symbolengine(@FOLDER@)symbolengine") + std::string(path), std::ios::binary);
        if (in)
        {
            thread_local std::string read;
            read.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            return read;
        }
    }
#endif

    for (const auto& file : files)
        if (file.bytes != nullptr && file.path == path)
            return std::string_view(reinterpret_cast<const char*>(file.bytes), file.size);
    return std::nullopt;
}
]=])

string(CONFIGURE "${template}" source @ONLY)
file(WRITE "${OUTPUT}.new" "${source}")

# Only a change is written, so an unchanged folder doesn't rebuild what uses it.
file(COPY_FILE "${OUTPUT}.new" "${OUTPUT}" ONLY_IF_DIFFERENT)
file(REMOVE "${OUTPUT}.new")
