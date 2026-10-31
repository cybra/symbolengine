# The net module fetches on a Mac and Windows, and elsewhere says it can't, so this is built only there. It has no test,
# as it needs the network.
if(APPLE OR WIN32)
    add_executable(Fetch Fetch.cpp)
    target_link_libraries(Fetch PRIVATE SymbolEngine::net)
    symbolengine_set_warnings(Fetch)
endif()
