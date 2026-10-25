#include <symbolengine/core/NoDenormals.h>

#include <catch2/catch_test_macros.hpp>

#include <cfenv>
#include <limits>

using namespace symbolengine;

namespace
{
// Kept out of the compiler's reach, so the product is made at run time under the thread's own settings.
volatile float smallest = std::numeric_limits<float>::min();
volatile float tenth = 0.1f;
}

TEST_CASE("NoDenormals flushes denormal results to zero and leaves the rounding mode alone")
{
    const auto rounding = std::fegetround();
    CHECK(smallest * tenth != 0.0f);

    {
        const NoDenormals guard;
        CHECK(smallest * tenth == 0.0f);
        CHECK(std::fegetround() == rounding);
    }

    CHECK(smallest * tenth != 0.0f);
}
