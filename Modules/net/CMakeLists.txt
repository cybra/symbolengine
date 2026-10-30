# Each system's own client: NSURLSession on a Mac and WinHTTP on Windows; other systems say they can't yet.
set(sources src/Http.cpp)
set(libraries)

if(APPLE)
    list(APPEND sources src/Http_mac.mm)
    list(APPEND libraries "-framework Foundation")
elseif(WIN32)
    list(APPEND sources src/Http_windows.cpp)
    list(APPEND libraries winhttp)
else()
    list(APPEND sources src/Http_other.cpp)
endif()

symbolengine_add_module(net SOURCES ${sources} DEPENDS core LIBRARIES ${libraries})

if(SYMBOLENGINE_TESTS)
    symbolengine_add_tests(net SOURCES Tests/HttpTests.cpp)
endif()
