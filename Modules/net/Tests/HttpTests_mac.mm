#include <symbolengine/net/Http.h>

#include <catch2/catch_test_macros.hpp>

#import <Foundation/Foundation.h>

#include <dlfcn.h>
#include <objc/runtime.h>
#include <string_view>

// Plugins built on different versions of this code can share a process, where a class has only one definition for each
// name, so whichever loaded first would serve them all.
TEST_CASE("This code's Objective-C classes have no name fixed when it was built")
{
    Dl_info here{};
    REQUIRE(dladdr(reinterpret_cast<const void*>(&symbolengine::http::available), &here) != 0);

    unsigned count = 0;
    const char** names = objc_copyClassNamesForImage(here.dli_fname, &count);
    for (unsigned i = 0; i < count; ++i)
        CHECK_FALSE(std::string_view(names[i]).starts_with("SymbolEngine"));
    free(names);
}
