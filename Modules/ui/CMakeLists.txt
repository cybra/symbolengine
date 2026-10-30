# The web view is the system's own: WKWebView on a Mac and WebView2 on Windows. Linux's, WebKitGTK, follows the first
# release.
if(NOT (APPLE OR WIN32))
    message(STATUS "SymbolEngine: the ui module is built on Mac and Windows; Linux follows the first release")
    return()
endif()

include(FetchContent)
include(SymbolEngineResources)

# CHOC, pinned by checksum, with an option added for where WebView2 keeps its data (cmake/PatchChoc.cmake). On Windows its web view carries Microsoft's WebView2Loader inside it, under Microsoft's
# BSD-style license, whose notice a program built with the ui module includes with its other notices.
# The patch's checksum goes with it, so a CHOC patched by an earlier version of it, as a kept build folder has, is
# patched again.
file(SHA256 ${PROJECT_SOURCE_DIR}/cmake/PatchChoc.cmake choc_patch)
FetchContent_Declare(symbolengine_choc
    URL https://github.com/Tracktion/choc/archive/8af031c2ba4f5723f4b0a5ca14e0b4984af15b0e.tar.gz
    URL_HASH SHA256=930607066e0120438e6452d6caac21f886bf0289ae6db4717b3ce892e4d09b8a
    PATCH_COMMAND ${CMAKE_COMMAND} -DFILE=choc/gui/choc_WebView.h -DPATCH=${choc_patch} -P ${PROJECT_SOURCE_DIR}/cmake/PatchChoc.cmake
    SOURCE_SUBDIR fetched-only)
FetchContent_MakeAvailable(symbolengine_choc)

if(APPLE)
    set(sources src/Dialogs_mac.mm src/MenuBar_mac.mm src/MessageLoop_mac.mm src/Native_mac.mm src/Snapshot_mac.mm src/Window_mac.mm)
    set(libraries "-framework WebKit" "-framework AppKit" "-framework Foundation" "-framework UniformTypeIdentifiers")
else()
    set(sources src/Dialogs_windows.cpp src/MenuBar_other.cpp src/MessageLoop_windows.cpp src/Native_windows.cpp src/Window_windows.cpp)
    set(libraries ole32 oleaut32 user32 shell32 version dwmapi advapi32)
endif()

symbolengine_add_module(ui
    SOURCES src/MessageLoop.cpp src/WebView.cpp src/Timer.cpp ${sources}
    DEPENDS core
    LIBRARIES ${libraries})

target_include_directories(symbolengine_ui SYSTEM PRIVATE ${symbolengine_choc_SOURCE_DIR})
symbolengine_add_files(symbolengine_ui FOLDER web FUNCTION symbolengine_ui_bridge_files)

if(SYMBOLENGINE_TESTS)
    symbolengine_add_tests(ui SOURCES Tests/DialogsTests.cpp Tests/MessageLoopTests.cpp Tests/WebViewTests.cpp Tests/WindowTests.cpp
                           ALONE "[webview]")
    target_include_directories(symbolengine_ui_tests SYSTEM PRIVATE ${symbolengine_choc_SOURCE_DIR})
    if(APPLE)
        target_sources(symbolengine_ui_tests PRIVATE Tests/MenuBarTests.mm)
        target_link_libraries(symbolengine_ui_tests PRIVATE "-framework ImageIO" "-framework CoreGraphics")
    endif()

    # A folder in the build folder for the live-files test to change.
    set(live ${CMAKE_CURRENT_BINARY_DIR}/live-files)
    if(NOT EXISTS ${live}/greeting.txt)
        file(WRITE ${live}/greeting.txt "built in")
    endif()
    symbolengine_add_files(symbolengine_ui_tests FOLDER ${live} FUNCTION liveTestFiles)
    target_compile_definitions(symbolengine_ui_tests PRIVATE "SYMBOLENGINE_LIVE_TEST_FOLDER=\"${live}\"")
endif()
