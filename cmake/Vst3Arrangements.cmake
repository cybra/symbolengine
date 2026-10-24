# cmake -DVALIDATOR=<validator> -DPLUGIN=<VST3 folder> -DEXPECTED=<In>Out,...> -P Vst3Arrangements.cmake
#
# Runs Steinberg's validator's extended tests, which ask the plugin for one speaker arrangement after another, and
# passes when the plugin took exactly those expected, named as the validator names them, such as Mono>50. clap-wrapper
# says "AUDIO in" each time it sets a plugin's buses up, which it does only once the plugin has taken an arrangement.

# The validator prints on standard output and clap-wrapper on standard error, so both go to one file, where each lands
# in the order it was written; read through two pipes, an arrangement and its "AUDIO in" could arrive the other way round.
string(SHA1 key "${PLUGIN}")
set(log "${CMAKE_CURRENT_BINARY_DIR}/vst3-arrangements-${key}.txt")
execute_process(COMMAND ${VALIDATOR} -e ${PLUGIN} RESULT_VARIABLE result OUTPUT_FILE ${log} ERROR_FILE ${log})
file(READ ${log} output)
file(REMOVE ${log})

if(NOT result EQUAL 0)
    message("${output}")
    message(FATAL_ERROR "The validator failed: ${result}")
endif()

# Semicolons would split the output into a list.
string(REPLACE ";" "," output "${output}")

string(REGEX MATCHALL "\\[In: [^:\n]+: [0-9]+ Channels, Out: [^:\n]+: [0-9]+ Channels\\]\n\tAUDIO in" found "${output}")
string(REGEX MATCHALL "\\[In: [^:\n]+: [0-9]+ Channels, Out: [^:\n]+: [0-9]+ Channels\\]" asked "${output}")

set(taken)
foreach(entry IN LISTS found)
    string(REGEX REPLACE "^\\[In: ([^:]+): [0-9]+ Channels, Out: ([^:]+): .*" "\\1>\\2" pair "${entry}")
    list(APPEND taken "${pair}")
endforeach()
list(REMOVE_DUPLICATES taken)
list(SORT taken)

string(REPLACE "," ";" expected "${EXPECTED}")
list(SORT expected)

list(LENGTH asked asked_count)
list(LENGTH taken taken_count)
message("The validator asked for ${asked_count} arrangements; the plugin took ${taken_count} different ones: ${taken}")

if(asked_count EQUAL 0)
    message(FATAL_ERROR "The validator asked for no speaker arrangements, so this checked nothing")
endif()
if(NOT taken STREQUAL expected)
    message(FATAL_ERROR "The plugin should have taken: ${expected}")
endif()
