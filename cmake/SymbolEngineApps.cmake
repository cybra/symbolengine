include_guard(GLOBAL)

set(SYMBOLENGINE_APP_TEMPLATES ${CMAKE_CURRENT_LIST_DIR} CACHE INTERNAL "")

# symbolengine_add_app(<target> NAME <name> IDENTIFIER <identifier> [VERSION <x.y.z>] [COMPANY <name>]
#                      [COPYRIGHT <text>] [ICON_NAME <name>] [DOCUMENT_EXTENSIONS <extension>...]
#                      [MICROPHONE_TEXT <text>] [PLIST_ENTRIES <xml>])
#
# Makes an executable target a desktop program on SymbolEngine's app module, named <name> to the system: on a Mac an
# application bundle with its Info.plist, written from these arguments, and on Windows a program with a window of its
# own, no console, and its version information, which the file's Properties show. The program's own main() is its
# entry on both systems, and it has SYMBOLENGINE_APP_NAME and SYMBOLENGINE_APP_VERSION defined as strings.
#
# IDENTIFIER is its bundle identifier, such as com.example.editor. VERSION is the project's unless given. COMPANY and
# COPYRIGHT go in the version information and the Info.plist. ICON_NAME names the icon the program puts in its bundle's
# Resources itself, as AppIcon.icns or an icon compiled by actool; DOCUMENT_EXTENSIONS lists the kinds of file the
# program opens, which the Finder then opens with it; MICROPHONE_TEXT is what a Mac shows when the program first asks
# for the microphone; and PLIST_ENTRIES is further XML for the Info.plist's dictionary.
#
# Where the program lands is the project's to choose, as for any executable, with RUNTIME_OUTPUT_DIRECTORY. The
# Info.plist written is in the target's SYMBOLENGINE_APP_INFO_PLIST property, for a build step that depends on it.
function(symbolengine_add_app target)
    cmake_parse_arguments(PARSE_ARGV 1 app ""
        "NAME;IDENTIFIER;VERSION;COMPANY;COPYRIGHT;ICON_NAME;MICROPHONE_TEXT;PLIST_ENTRIES" "DOCUMENT_EXTENSIONS")

    if(NOT app_NAME)
        message(FATAL_ERROR "symbolengine_add_app(${target}) needs a NAME, as the system shows it")
    endif()
    if(NOT app_IDENTIFIER)
        message(FATAL_ERROR "symbolengine_add_app(${target}) needs an IDENTIFIER, such as com.example.editor")
    endif()
    if(NOT app_VERSION)
        set(app_VERSION ${PROJECT_VERSION})
    endif()
    if(NOT app_VERSION MATCHES "^[0-9]+(\\.[0-9]+)*$")
        message(FATAL_ERROR "symbolengine_add_app(${target}): VERSION must be numbers separated by dots, such as 1.2.3")
    endif()

    target_link_libraries(${target} PRIVATE SymbolEngine::app)
    target_compile_definitions(${target} PRIVATE
        "SYMBOLENGINE_APP_NAME=\"${app_NAME}\""
        "SYMBOLENGINE_APP_VERSION=\"${app_VERSION}\"")
    set_target_properties(${target} PROPERTIES OUTPUT_NAME "${app_NAME}")

    if(APPLE)
        set(APP_NAME "${app_NAME}")
        set(APP_IDENTIFIER "${app_IDENTIFIER}")
        set(APP_VERSION "${app_VERSION}")
        set(APP_COPYRIGHT "${app_COPYRIGHT}")
        set(APP_MINIMUM_SYSTEM "${CMAKE_OSX_DEPLOYMENT_TARGET}")
        if(NOT APP_MINIMUM_SYSTEM)
            set(APP_MINIMUM_SYSTEM 11.0)
        endif()

        set(APP_EXTRA_ENTRIES "")
        if(app_ICON_NAME)
            string(APPEND APP_EXTRA_ENTRIES
                "    <key>CFBundleIconFile</key>\n    <string>${app_ICON_NAME}</string>\n"
                "    <key>CFBundleIconName</key>\n    <string>${app_ICON_NAME}</string>\n")
        endif()
        if(app_MICROPHONE_TEXT)
            string(APPEND APP_EXTRA_ENTRIES
                "    <key>NSMicrophoneUsageDescription</key>\n    <string>${app_MICROPHONE_TEXT}</string>\n")
        endif()
        if(app_DOCUMENT_EXTENSIONS)
            string(APPEND APP_EXTRA_ENTRIES "    <key>CFBundleDocumentTypes</key>\n    <array>\n")
            foreach(extension IN LISTS app_DOCUMENT_EXTENSIONS)
                string(APPEND APP_EXTRA_ENTRIES
                    "        <dict>\n"
                    "            <key>CFBundleTypeExtensions</key>\n            <array><string>${extension}</string></array>\n"
                    "            <key>CFBundleTypeName</key>\n            <string>${extension}</string>\n"
                    "            <key>CFBundleTypeRole</key>\n            <string>Editor</string>\n"
                    "            <key>LSHandlerRank</key>\n            <string>Default</string>\n"
                    "        </dict>\n")
            endforeach()
            string(APPEND APP_EXTRA_ENTRIES "    </array>\n")
        endif()
        if(app_PLIST_ENTRIES)
            string(APPEND APP_EXTRA_ENTRIES "${app_PLIST_ENTRIES}\n")
        endif()

        set(plist ${CMAKE_CURRENT_BINARY_DIR}/${target}_Info.plist)
        configure_file(${SYMBOLENGINE_APP_TEMPLATES}/AppInfo.plist.in ${plist} @ONLY)
        set_target_properties(${target} PROPERTIES
            MACOSX_BUNDLE TRUE
            MACOSX_BUNDLE_INFO_PLIST ${plist}
            SYMBOLENGINE_APP_INFO_PLIST ${plist})
    elseif(WIN32)
        # The version as the resource wants it: four numbers.
        string(REPLACE "." ";" parts "${app_VERSION}")
        list(LENGTH parts count)
        while(count LESS 4)
            list(APPEND parts 0)
            math(EXPR count "${count} + 1")
        endwhile()
        list(SUBLIST parts 0 4 parts)
        string(REPLACE ";" "," APP_VERSION_COMMAS "${parts}")
        set(APP_NAME "${app_NAME}")
        set(APP_VERSION "${app_VERSION}")
        set(APP_COMPANY "${app_COMPANY}")
        set(APP_COPYRIGHT "${app_COPYRIGHT}")

        set(rc ${CMAKE_CURRENT_BINARY_DIR}/${target}_Version.rc)
        configure_file(${SYMBOLENGINE_APP_TEMPLATES}/AppVersion.rc.in ${rc} @ONLY)
        target_sources(${target} PRIVATE ${rc})
        set_target_properties(${target} PROPERTIES WIN32_EXECUTABLE TRUE)
        # A program with no console starts at main all the same.
        if(MSVC)
            target_link_options(${target} PRIVATE /ENTRY:mainCRTStartup)
        endif()
    endif()
endfunction()
