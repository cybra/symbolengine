// Any bytes as a gzip file: it unpacks or says it can't, never past its limit, and any bytes packed come back as they were.

#include "Fuzz.h"

#include <symbolengine/core/Gzip.h>

using namespace symbolengine;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    const auto bytes = fuzz::text(data, size);
    constexpr std::size_t limit = 1u << 20;

    if (const auto unpacked = gzip::decompress(bytes, limit); unpacked && unpacked->size() > limit)
        fuzz::broken();

    if (size <= 1u << 16)
    {
        const auto back = gzip::decompress(gzip::compress(bytes));
        if (!back || *back != bytes)
            fuzz::broken();
    }

    return 0;
}
