add_executable(Hello Hello.cpp)
symbolengine_add_app(Hello
    NAME "SymbolEngine Hello"
    IDENTIFIER dev.symbolengine.examples.hello
    COMPANY "Cyberhost LLC"
    COPYRIGHT "Copyright 2026 Cyberhost LLC"
    DOCUMENT_EXTENSIONS txt)

symbolengine_add_files(Hello FOLDER web FUNCTION helloFiles)
symbolengine_set_warnings(Hello)
