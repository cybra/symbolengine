#include "symbolengine/files/BroadcastWave.h"

#include "Bytes.h"

#include <algorithm>

namespace symbolengine::files
{

namespace
{
// The fixed part of a bext chunk, the coding history following it: description 256, originator 32, its reference 32,
// date 10, time 8, time reference 8, version 2, UMID 64, five loudness values of 2 each, and 180 reserved.
constexpr std::size_t fixedSize = 602;
constexpr std::size_t timeReferenceAt = 338;
constexpr std::size_t versionAt = 346;
constexpr std::size_t umidAt = 348;
constexpr std::size_t loudnessAt = 412;

std::string field(const std::string& bytes, std::size_t at, std::size_t size)
{
    const auto text = bytes.substr(at, size);
    return text.substr(0, text.find('\0'));
}

void addField(std::string& bytes, const std::string& text, std::size_t size)
{
    bytes += text.substr(0, size);
    bytes.append(size - std::min(size, text.size()), '\0');
}
}

std::optional<BroadcastExtension> broadcastExtension(const std::vector<AudioChunk>& chunks)
{
    const auto found = std::find_if(chunks.begin(), chunks.end(), [](const AudioChunk& c) { return c.id == "bext"; });
    if (found == chunks.end() || found->bytes.size() < umidAt)
        return std::nullopt;

    const auto& bytes = found->bytes;
    const auto* p = detail::bytesOf(bytes);

    BroadcastExtension extension;
    extension.description = field(bytes, 0, 256);
    extension.originator = field(bytes, 256, 32);
    extension.originatorReference = field(bytes, 288, 32);
    extension.originationDate = field(bytes, 320, 10);
    extension.originationTime = field(bytes, 330, 8);
    extension.timeReference = detail::readLittle64(p + timeReferenceAt);
    extension.version = detail::readLittle16(p + versionAt);

    if (bytes.size() >= umidAt + 64)
    {
        const auto umid = bytes.substr(umidAt, 64);
        if (umid.find_first_not_of('\0') != std::string::npos)
            extension.umid = umid;
    }

    if (bytes.size() > fixedSize)
        extension.codingHistory = field(bytes, fixedSize, bytes.size() - fixedSize);

    return extension;
}

AudioChunk bextChunk(const BroadcastExtension& extension)
{
    std::string bytes;
    bytes.reserve(fixedSize + extension.codingHistory.size());

    addField(bytes, extension.description, 256);
    addField(bytes, extension.originator, 32);
    addField(bytes, extension.originatorReference, 32);
    addField(bytes, extension.originationDate, 10);
    addField(bytes, extension.originationTime, 8);
    detail::addLittle64(bytes, extension.timeReference);
    detail::addLittle16(bytes, extension.version);
    addField(bytes, extension.umid, 64);

    // Version 2's loudness values, not measured; earlier versions keep the space reserved.
    for (int i = 0; i < 5; ++i)
        detail::addLittle16(bytes, extension.version >= 2 ? 0x7FFF : 0);
    bytes.append(fixedSize - loudnessAt - 10, '\0');

    bytes += extension.codingHistory;
    return {"bext", std::move(bytes)};
}

}
