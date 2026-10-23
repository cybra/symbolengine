include_guard(GLOBAL)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(package_folder ${CMAKE_INSTALL_LIBDIR}/cmake/SymbolEngine)

get_property(SYMBOLENGINE_MODULE_LIST GLOBAL PROPERTY SYMBOLENGINE_MODULES)

foreach(module IN LISTS SYMBOLENGINE_MODULE_LIST)
    install(TARGETS symbolengine_${module} EXPORT SymbolEngineTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(DIRECTORY ${PROJECT_SOURCE_DIR}/Modules/${module}/include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endforeach()

install(EXPORT SymbolEngineTargets NAMESPACE SymbolEngine:: DESTINATION ${package_folder})

configure_package_config_file(${PROJECT_SOURCE_DIR}/cmake/SymbolEngineConfig.cmake.in
    ${PROJECT_BINARY_DIR}/SymbolEngineConfig.cmake
    INSTALL_DESTINATION ${package_folder})

# Before 1.0 a minor version may change the interface, so find_package accepts only the same minor version.
write_basic_package_version_file(${PROJECT_BINARY_DIR}/SymbolEngineConfigVersion.cmake
    COMPATIBILITY SameMinorVersion)

install(FILES ${PROJECT_BINARY_DIR}/SymbolEngineConfig.cmake ${PROJECT_BINARY_DIR}/SymbolEngineConfigVersion.cmake
    DESTINATION ${package_folder})
install(FILES ${PROJECT_SOURCE_DIR}/LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/SymbolEngine)
