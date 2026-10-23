include_guard(GLOBAL)

include(GNUInstallDirs)

# Adds the compiler warnings SymbolEngine's own code is held to.
function(symbolengine_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
        if(SYMBOLENGINE_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wnon-virtual-dtor -Woverloaded-virtual
            -Wcast-align -Wformat=2 -Wimplicit-fallthrough)
        if(SYMBOLENGINE_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

# symbolengine_add_module(<name> [NOT_INSTALLED] SOURCES <files...> [DEPENDS <modules...>] [LIBRARIES <libraries...>])
#
# Adds the module <name> as the static library SymbolEngine::<name>, its public headers in include/symbolengine/<name>.
# DEPENDS names the SymbolEngine modules it is built on, and LIBRARIES anything else it links to. NOT_INSTALLED leaves
# it out of what find_package finds.
function(symbolengine_add_module name)
    cmake_parse_arguments(PARSE_ARGV 1 module "NOT_INSTALLED" "" "SOURCES;DEPENDS;LIBRARIES")

    set(target symbolengine_${name})
    add_library(${target} STATIC ${module_SOURCES})
    add_library(SymbolEngine::${name} ALIAS ${target})
    # Linked into plugins, where nothing but a plugin's entry point may be visible outside it.
    set_target_properties(${target} PROPERTIES
        EXPORT_NAME ${name} POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)

    target_compile_features(${target} PUBLIC cxx_std_20)
    target_include_directories(${target} PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

    foreach(dependency IN LISTS module_DEPENDS)
        target_link_libraries(${target} PUBLIC symbolengine_${dependency})
    endforeach()
    target_link_libraries(${target} PRIVATE ${module_LIBRARIES})

    symbolengine_set_warnings(${target})
    if(NOT module_NOT_INSTALLED)
        set_property(GLOBAL APPEND PROPERTY SYMBOLENGINE_MODULES ${name})
    endif()
endfunction()
