# The web view is the system's own: WKWebView on a Mac and WebView2 on Windows. Linux's, WebKitGTK, follows the first
# release.
if(NOT (APPLE OR WIN32))
    message(STATUS "SymbolEngine: the ui module is built on Mac and Windows; Linux follows the first release")
    return()
endif()

include(FetchContent)
include(SymbolEngineResources)

# CHOC, pinned by checksum. On Windows its web view carries Microsoft's WebView2Loader inside it, under Microsoft's
# BSD-style license, whose notice a program built with the ui module includes with its other notices.
FetchContent_Declare(symbolengine_choc
    URL https://github.com/Tracktion/choc/archive/8af031c2ba4f5723f4b0a5ca14e0b4984af15b0e.tar.gz
    URL_HASH SHA256=930607066e0120438e6452d6caac21f886bf0289ae6db4717b3ce892e4d09b8a
    SOURCE_SUBDIR fetched-only)
FetchContent_MakeAvailable(symbolengine_choc)

if(APPLE)
    set(libraries "-framework WebKit" "-framework AppKit" "-framework Foundation")
else()
    set(libraries ole32 oleaut32 user32 shell32 version)
endif()

symbolengine_add_module(ui
    SOURCES src/WebView.cpp
    DEPENDS core
    LIBRARIES ${libraries})

target_include_directories(symbolengine_ui SYSTEM PRIVATE ${symbolengine_choc_SOURCE_DIR})
symbolengine_add_files(symbolengine_ui FOLDER web FUNCTION symbolengine_ui_bridge_files)

if(SYMBOLENGINE_TESTS)
    symbolengine_add_tests(ui SOURCES Tests/WebViewTests.cpp)
    target_include_directories(symbolengine_ui_tests SYSTEM PRIVATE ${symbolengine_choc_SOURCE_DIR})
endif()
