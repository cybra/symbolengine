// Any bytes as a WAV's broadcast extension: it reads or says it can't, and what reads writes as what reads the same.

#include "Fuzz.h"

#include <symbolengine/files/BroadcastWave.h>

#include <string>
#include <vector>

using namespace symbolengine;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    const std::vector<AudioChunk> chunks{AudioChunk{"bext", std::string(fuzz::text(data, size))}};

    if (const auto extension = files::broadcastExtension(chunks))
    {
        const auto written = files::bextChunk(*extension);
        const auto again = files::broadcastExtension({written});
        if (!again || files::bextChunk(*again).bytes != written.bytes)
            fuzz::broken();
    }

    return 0;
}
