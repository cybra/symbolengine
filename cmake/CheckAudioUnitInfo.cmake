# cmake -DPLIST=<Info.plist> -DMANUFACTURER=<code> -P CheckAudioUnitInfo.cmake
#
# Passes when an Audio Unit's Info.plist names its component and maker, as macOS needs to find it.

file(READ "${PLIST}" plist)

if(NOT plist MATCHES "<key>AudioComponents</key>")
    message(FATAL_ERROR "${PLIST} has no AudioComponents, so macOS won't find the Audio Unit")
endif()
if(NOT plist MATCHES "<string>${MANUFACTURER}</string>")
    message(FATAL_ERROR "${PLIST} doesn't name the manufacturer ${MANUFACTURER}")
endif()
