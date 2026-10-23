# cmake -DCOMPONENT=<bundle> -DTYPE=<type> -DSUBTYPE=<subtype> -DMANUFACTURER=<code> -P Auval.cmake
#
# Installs an Audio Unit for the user running it, has the system find it again, and runs Apple's auval on it.

get_filename_component(name "${COMPONENT}" NAME)
set(installed "$ENV{HOME}/Library/Audio/Plug-Ins/Components/${name}")

file(REMOVE_RECURSE "${installed}")
file(COPY "${COMPONENT}" DESTINATION "$ENV{HOME}/Library/Audio/Plug-Ins/Components")

# The registrar caches what it found; stopping it makes the system look again.
execute_process(COMMAND killall -9 AudioComponentRegistrar OUTPUT_QUIET ERROR_QUIET)

execute_process(COMMAND auval -strict -v ${TYPE} ${SUBTYPE} ${MANUFACTURER}
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output)
message("${output}")

if(NOT result EQUAL 0)
    message(FATAL_ERROR "auval failed: ${result}")
endif()
