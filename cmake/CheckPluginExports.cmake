# cmake -DBINARY=<a CLAP plugin's binary> -P CheckPluginExports.cmake
#
# Passes when a plugin's binary makes nothing visible outside it but its entry point, so that nothing of it can be
# confused with another plugin's in one process.

execute_process(COMMAND nm -gU "${BINARY}" OUTPUT_VARIABLE exported RESULT_VARIABLE failed)
if(failed)
    message(FATAL_ERROR "nm couldn't read ${BINARY}")
endif()

string(REGEX REPLACE "[^\n]* ([^ \n]+)\n" "\\1;" names "${exported}")
list(REMOVE_ITEM names "_clap_entry")
list(FILTER names EXCLUDE REGEX "^$")

# The standard library's own inline functions, which some of its versions leave visible whatever is asked, and which
# each binary resolves to its own copy.
list(FILTER names EXCLUDE REGEX "^__ZNK?St|^__ZSt")
if(names)
    message(FATAL_ERROR "${BINARY} makes visible more than its entry point: ${names}")
endif()
