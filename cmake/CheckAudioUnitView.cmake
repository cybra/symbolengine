# cmake -DBINARY=<an Audio Unit's binary> -DSUBTYPE=<its code> -P CheckAudioUnitView.cmake
#
# Passes when the Objective-C class of an Audio Unit's view is named by the unit's code as it is spelled, capitals
# included, so that it is the unit's own and not one another's in the process.

execute_process(COMMAND nm -U "${BINARY}" OUTPUT_VARIABLE symbols RESULT_VARIABLE failed)
if(failed)
    message(FATAL_ERROR "nm couldn't read ${BINARY}")
endif()

string(REGEX MATCH "_OBJC_CLASS_\\$_wrapAsAUV2_inst_cocoaUI_[A-Za-z0-9_]*" view "${symbols}")
if(NOT view)
    message(FATAL_ERROR "${BINARY} has no view class")
endif()
string(FIND "${view}" "_${SUBTYPE}_" found)
if(found EQUAL -1)
    message(FATAL_ERROR "The view class ${view} doesn't spell the code ${SUBTYPE}")
endif()
