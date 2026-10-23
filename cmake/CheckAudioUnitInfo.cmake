# cmake -DPLIST=<Info.plist> -DMANUFACTURER=<code> -DNAME=<name> -P CheckAudioUnitInfo.cmake
#
# Passes when an Audio Unit's Info.plist names its component and maker, as macOS needs to find it, and gives it the
# name hosts list it by, such as "SymbolEngine: Gain".

file(READ "${PLIST}" plist)

if(NOT plist MATCHES "<key>AudioComponents</key>")
    message(FATAL_ERROR "${PLIST} has no AudioComponents, so macOS won't find the Audio Unit")
endif()
if(NOT plist MATCHES "<string>${MANUFACTURER}</string>")
    message(FATAL_ERROR "${PLIST} doesn't name the manufacturer ${MANUFACTURER}")
endif()
string(FIND "${plist}" "<key>name</key>\n        <string>${NAME}</string>" named)
if(named EQUAL -1)
    message(FATAL_ERROR "${PLIST} doesn't give the Audio Unit the name ${NAME}")
endif()
