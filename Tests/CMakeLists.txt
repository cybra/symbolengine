# A project outside SymbolEngine takes it in both ways the README describes: FetchContent from its source, and
# find_package once it is installed. Each builds Tests/Consumer and runs what it built.

set(consumer_source ${CMAKE_CURRENT_SOURCE_DIR}/Consumer)
set(consumer_build ${CMAKE_CURRENT_BINARY_DIR}/consumer)

set(consumer_options
    -DCMAKE_BUILD_TYPE=$<CONFIG>
    -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
    "-DCMAKE_CXX_FLAGS=${CMAKE_CXX_FLAGS}"
    "-DCMAKE_EXE_LINKER_FLAGS=${CMAKE_EXE_LINKER_FLAGS}")

if(APPLE AND CMAKE_OSX_DEPLOYMENT_TARGET)
    list(APPEND consumer_options -DCMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET})
endif()

set(consumer_generator --build-generator ${CMAKE_GENERATOR})
if(CMAKE_GENERATOR_PLATFORM)
    list(APPEND consumer_generator --build-generator-platform ${CMAKE_GENERATOR_PLATFORM})
endif()
if(CMAKE_MAKE_PROGRAM)
    list(APPEND consumer_generator --build-makeprogram ${CMAKE_MAKE_PROGRAM})
endif()

add_test(NAME "consumer: FetchContent"
    COMMAND ${CMAKE_CTEST_COMMAND}
        --build-and-test ${consumer_source} ${consumer_build}/fetch
        ${consumer_generator}
        --build-config $<CONFIG>
        --build-options ${consumer_options} -DCONSUME=fetch -DSYMBOLENGINE_SOURCE=${PROJECT_SOURCE_DIR}
        --test-command consumer)

add_test(NAME "consumer: install"
    COMMAND ${CMAKE_COMMAND} --install ${PROJECT_BINARY_DIR} --prefix ${consumer_build}/prefix --config $<CONFIG>)
set_tests_properties("consumer: install" PROPERTIES FIXTURES_SETUP installed)

add_test(NAME "consumer: find_package"
    COMMAND ${CMAKE_CTEST_COMMAND}
        --build-and-test ${consumer_source} ${consumer_build}/package
        ${consumer_generator}
        --build-config $<CONFIG>
        --build-options ${consumer_options} -DCONSUME=package -DCMAKE_PREFIX_PATH=${consumer_build}/prefix
        --test-command consumer)
set_tests_properties("consumer: find_package" PROPERTIES FIXTURES_REQUIRED installed)
