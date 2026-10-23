# cmake -DPROGRAM=<program> -DARGUMENT=<argument> -DEXPECT=<text> -P RunAndExpect.cmake
#
# Runs a program with one argument, and passes when it succeeds and says the text expected.

execute_process(COMMAND ${PROGRAM} ${ARGUMENT} RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output)
message("${output}")

if(NOT result EQUAL 0)
    message(FATAL_ERROR "${PROGRAM} failed: ${result}")
endif()

string(FIND "${output}" "${EXPECT}" found)
if(found EQUAL -1)
    message(FATAL_ERROR "${PROGRAM} succeeded, but never said: ${EXPECT}")
endif()
