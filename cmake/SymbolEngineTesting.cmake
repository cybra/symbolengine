include_guard(GLOBAL)

include(FetchContent)

FetchContent_Declare(Catch2
    URL https://github.com/catchorg/Catch2/archive/refs/tags/v3.8.1.tar.gz
    URL_HASH SHA256=18b3f70ac80fccc340d8c6ff0f339b2ae64944782f8d2fca2bd705cf47cadb79
    SYSTEM)

# Catch2 has to be built to the same standard as the tests, or the parts it offers only from C++17 go missing.
set(CMAKE_CXX_STANDARD 20)
FetchContent_MakeAvailable(Catch2)

include(Catch)

# symbolengine_add_tests(<module> SOURCES <files...>)
#
# Adds the test program for a module, each of its test cases a test of its own for ctest.
function(symbolengine_add_tests module)
    cmake_parse_arguments(PARSE_ARGV 1 tests "" "ALONE" "SOURCES")

    set(target symbolengine_${module}_tests)
    add_executable(${target} ${tests_SOURCES})
    target_link_libraries(${target} PRIVATE SymbolEngine::${module} Catch2::Catch2WithMain)
    symbolengine_set_warnings(${target})
    # Tests matching ALONE, a Catch2 test spec such as [timing] or [webview], or tags joined by commas, any of which
    # matches, run with no other test beside them taking a crowded runner's cores: those that measure time on the clock,
    # those that open a web view, whose engine starts processes of its own, which two or three at once on a crowded
    # runner took longer than the tests wait, and those that open many windows, which slow the tests beside them.
    if(tests_ALONE AND CMAKE_CXX_FLAGS MATCHES "-fsanitize=thread")
        # Ten times as slow, what measures time on the clock or starts many processes at once isn't for this build,
        # which is here to find races.
        string(REPLACE "," ";" alone_tags "${tests_ALONE}")
        set(others "")
        foreach(tag IN LISTS alone_tags)
            string(APPEND others "~${tag}")
        endforeach()
        catch_discover_tests(${target} TEST_PREFIX "${module}: " TEST_SPEC "${others}" DISCOVERY_MODE PRE_TEST)
    elseif(tests_ALONE)
        string(REPLACE "," ";" alone_tags "${tests_ALONE}")
        set(others "")
        foreach(tag IN LISTS alone_tags)
            string(APPEND others "~${tag}")
        endforeach()
        catch_discover_tests(${target} TEST_PREFIX "${module}: " TEST_SPEC "${others}" DISCOVERY_MODE PRE_TEST)
        catch_discover_tests(${target} TEST_PREFIX "${module}: " TEST_SPEC "${tests_ALONE}" DISCOVERY_MODE PRE_TEST
                             PROPERTIES RUN_SERIAL TRUE)
    else()
        catch_discover_tests(${target} TEST_PREFIX "${module}: " DISCOVERY_MODE PRE_TEST)
    endif()
endfunction()
