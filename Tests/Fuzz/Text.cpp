// Any bytes as JSON, as XML and as a VST3 manifest's JSON would be: each parses or says it can't, without exhausting the
// stack or reading out of bounds, and what parses writes as what parses to the same.

#include "Fuzz.h"

#include <symbolengine/core/Json.h>
#include <symbolengine/core/Xml.h>

using namespace symbolengine;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    const auto text = fuzz::text(data, size);

    if (const auto value = json::tryParse(text))
    {
        const auto once = json::write(*value);
        const auto again = json::tryParse(once);
        if (!again || json::write(*again) != once)
            fuzz::broken();
    }

    if (const auto element = xml::tryParse(text))
    {
        const auto once = xml::write(*element);
        const auto again = xml::tryParse(once);
        if (!again || xml::write(*again) != once)
            fuzz::broken();
    }

    return 0;
}
