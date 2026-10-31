#include "Bytes.h"
#include "Formats.h"
#include "Samples.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>

// AIFF and AIFF-C, as Apple describes them: big-endian chunks, the sample rate an 80-bit extended float, and in an
// AIFF-C file a compression type naming how the samples are kept, of which those that are plain samples are read:
// whole numbers of either byte order, floats, and unsigned 8-bit. Floats are written as AIFF-C, whole numbers as AIFF.
namespace symbolengine::detail
{

namespace
{
constexpr std::uint64_t biggestChunkKept = 64 * 1024 * 1024;
constexpr std::uint64_t biggestSize = 0xFFFFFFFF;

// An 80-bit IEEE 754 extended float, as COMM holds the sample rate.
double fromExtended(const unsigned char* p)
{
    const auto exponent = static_cast<int>((p[0] & 0x7F) << 8 | p[1]);
    std::uint64_t mantissa = 0;
    for (int i = 0; i < 8; ++i)
        mantissa = mantissa << 8 | p[2 + i];

    if (exponent == 0 && mantissa == 0)
        return 0.0;
    if (exponent == 0x7FFF)
        return 0.0;

    const auto value = std::ldexp(static_cast<double>(mantissa), exponent - 16383 - 63);
    return (p[0] & 0x80) != 0 ? -value : value;
}

void addExtended(std::string& bytes, double value)
{
    unsigned char out[10]{};
    if (value > 0.0 && std::isfinite(value))
    {
        int exponent = 0;
        const auto fraction = std::frexp(value, &exponent);     // value = fraction * 2^exponent, 0.5 <= fraction < 1
        const auto mantissa = static_cast<std::uint64_t>(std::ldexp(fraction, 64));
        const auto biased = static_cast<std::uint32_t>(exponent - 1 + 16383);
        out[0] = static_cast<unsigned char>(biased >> 8 & 0x7F);
        out[1] = static_cast<unsigned char>(biased & 0xFF);
        for (int i = 0; i < 8; ++i)
            out[2 + i] = static_cast<unsigned char>(mantissa >> (8 * (7 - i)));
    }
    bytes.append(reinterpret_cast<const char*>(out), sizeof out);
}

// How an AIFF-C compression type keeps its samples, or nothing for a compressed one.
std::optional<Encoding> encodingFor(const std::string& type, int bits)
{
    const auto bytes = (bits + 7) / 8;
    const auto whole = [bytes](Encoding one, Encoding two, Encoding three, Encoding four) -> std::optional<Encoding>
    {
        switch (bytes)
        {
            case 1: return one;
            case 2: return two;
            case 3: return three;
            case 4: return four;
            default: return std::nullopt;
        }
    };

    if (type == "NONE" || type == "twos")
        return whole(Encoding::signed8, Encoding::big16, Encoding::big24, Encoding::big32);
    if (type == "sowt")
        return whole(Encoding::signed8, Encoding::little16, Encoding::little24, Encoding::little32);
    if (type == "fl32" || type == "FL32")
        return Encoding::bigFloat32;
    if (type == "fl64" || type == "FL64")
        return Encoding::bigFloat64;
    if (type == "raw ")
        return bytes == 1 ? std::optional<Encoding>(Encoding::unsigned8) : std::nullopt;
    if (type == "in24")
        return Encoding::big24;
    if (type == "in32")
        return Encoding::big32;
    if (type == "23ni")
        return Encoding::little24;
    if (type == "42ni")
        return Encoding::little32;
    return std::nullopt;
}

class AiffReader final : public AudioFileReader
{
public:
    AiffReader(std::unique_ptr<RandomAccessFile> f, const std::filesystem::path& p, AudioInfo info, Encoding e,
               std::uint64_t start)
        : file(std::move(f)), encoding(e), dataStart(start)
    {
        information = std::move(info);
        path = p;
        frameBytes = static_cast<std::size_t>(bytesPerSample(encoding)) * static_cast<std::size_t>(information.channels);
    }

private:
    std::size_t readFrames(std::uint64_t from, float* const* channels, std::size_t frames, std::error_code& error) override
    {
        constexpr std::size_t block = 65536;
        std::size_t done = 0;

        while (done < frames)
        {
            const auto now = std::min(block, frames - done);
            scratch.resize(now * frameBytes);
            const auto got = file->readAt(dataStart + (from + done) * frameBytes, scratch.data(), scratch.size(), error);
            const auto whole = got / frameBytes;
            decode(encoding, scratch.data(), information.channels, whole, channels, done);
            done += whole;

            if (error || whole < now)
                break;
        }

        return done;
    }

    std::unique_ptr<RandomAccessFile> file;
    Encoding encoding;
    std::uint64_t dataStart = 0;
    std::size_t frameBytes = 1;
    std::vector<unsigned char> scratch;
};

class AiffWriter final : public AudioFileWriter
{
public:
    AiffWriter(std::unique_ptr<RandomAccessFile> f, const std::filesystem::path& p, const AudioFileSettings& s, Encoding e)
        : file(std::move(f)), encoding(e)
    {
        written = s;
        path = p;
        frameBytes = static_cast<std::size_t>(bytesPerSample(encoding)) * static_cast<std::size_t>(written.channels);
    }

    ~AiffWriter() override
    {
        std::error_code ignored;
        finish(ignored);
    }

    void writeHeader(std::error_code& error)
    {
        const auto compressed = written.floatingPoint;

        std::string header = "FORM";
        addBig32(header, 0);
        header += compressed ? "AIFC" : "AIFF";

        if (compressed)
        {
            header += "FVER";
            addBig32(header, 4);
            addBig32(header, 0xA2805140);
        }

        std::string name = written.bits == 64 ? "64-bit floating point" : "32-bit floating point";
        header += "COMM";
        addBig32(header, compressed ? static_cast<std::uint32_t>(18 + 4 + 1 + name.size() + (name.size() % 2 == 0 ? 1 : 0)) : 18);
        addBig16(header, static_cast<std::uint32_t>(written.channels));
        framesAt = header.size();
        addBig32(header, 0);
        addBig16(header, static_cast<std::uint32_t>(written.bits));
        addExtended(header, written.sampleRate);
        if (compressed)
        {
            header += written.bits == 64 ? "fl64" : "fl32";
            header += static_cast<char>(name.size());
            header += name;
            if (name.size() % 2 == 0)
                header += '\0';
        }

        for (const auto& chunk : written.chunks)
        {
            if (chunk.id.size() != 4 || isLayoutChunk(chunk.id) || chunk.bytes.size() > biggestSize - 1)
                continue;
            header += chunk.id;
            addBig32(header, static_cast<std::uint32_t>(chunk.bytes.size()));
            header += chunk.bytes;
            if (chunk.bytes.size() % 2 != 0)
                header += '\0';
        }

        header += "SSND";
        soundSizeAt = header.size();
        addBig32(header, 0);
        addBig32(header, 0);
        addBig32(header, 0);
        dataStart = header.size();

        file->writeAt(0, header.data(), header.size(), error);

        // A file whose header couldn't be written is left as it is.
        if (error)
            finished = true;
    }

private:
    void writeFrames(const float* const* channels, std::size_t frames, std::error_code& error) override
    {
        const auto adding = static_cast<std::uint64_t>(frames) * frameBytes;
        if (dataStart + dataBytes + adding + 1 - 8 > biggestSize || count + frames > biggestSize)
        {
            error = AudioFileError::tooLong;
            return;
        }

        scratch.resize(frames * frameBytes);
        encode(encoding, channels, written.channels, frames, scratch.data());
        file->writeAt(dataStart + dataBytes, scratch.data(), scratch.size(), error);
        if (!error)
            dataBytes += scratch.size();
    }

    // The sizes for the samples so far, after the pad byte samples of odd length need, which the next samples write over.
    void writeSizes(std::error_code& error)
    {
        auto end = dataStart + dataBytes;
        if (dataBytes % 2 != 0)
        {
            const char pad = 0;
            file->writeAt(end, &pad, 1, error);
            ++end;
        }

        const auto put = [this, &error](std::uint64_t at, std::uint64_t value)
        {
            if (error)
                return;
            std::string bytes;
            addBig32(bytes, static_cast<std::uint32_t>(value));
            file->writeAt(at, bytes.data(), bytes.size(), error);
        };

        put(4, end - 8);
        put(framesAt, count);
        put(soundSizeAt, 8 + dataBytes);
    }

    void flushFile(std::error_code& error) override
    {
        writeSizes(error);
        if (!error)
            file->sync(error);
    }

    void finishFile(std::error_code& error) override
    {
        writeSizes(error);

        std::error_code closing;
        file->close(closing);
        if (!error)
            error = closing;
    }

    std::unique_ptr<RandomAccessFile> file;
    Encoding encoding;
    std::size_t frameBytes = 1;
    std::uint64_t dataStart = 0, dataBytes = 0;
    std::size_t framesAt = 0, soundSizeAt = 0;
    std::vector<unsigned char> scratch;
};
}

std::unique_ptr<AudioFileReader> openAiff(std::unique_ptr<RandomAccessFile> file, const std::filesystem::path& path,
                                          std::error_code& error)
{
    const auto fileSize = file->size(error);
    if (error)
        return nullptr;

    unsigned char start[12]{};
    file->readAt(0, start, sizeof start, error);
    if (error)
        return nullptr;

    const auto compressed = std::memcmp(start + 8, "AIFC", 4) == 0;

    AudioInfo info;
    info.format = "AIFF";
    std::optional<Encoding> encoding;
    bool haveCommon = false;
    std::uint64_t commonFrames = 0;
    std::optional<std::uint64_t> dataStart, dataSize;

    std::uint64_t position = 12;
    while (position + 8 <= fileSize)
    {
        unsigned char header[8]{};
        if (file->readAt(position, header, sizeof header, error) < sizeof header || error)
            break;
        if (!looksLikeId(header))
            break;

        const std::string id(reinterpret_cast<const char*>(header), 4);
        const auto body = position + 8;
        std::uint64_t size = readBig32(header + 4);

        if (id == "COMM")
        {
            unsigned char bytes[22]{};
            const auto got = file->readAt(body, bytes, static_cast<std::size_t>(std::min<std::uint64_t>(size, 22)), error);
            if (got < 18 || (compressed && got < 22))
            {
                error = AudioFileError::damaged;
                return nullptr;
            }

            info.channels = readBig16(bytes);
            commonFrames = readBig32(bytes + 2);
            info.bits = readBig16(bytes + 6);
            info.sampleRate = fromExtended(bytes + 8);
            const auto type = compressed ? std::string(reinterpret_cast<const char*>(bytes + 18), 4) : std::string("NONE");
            encoding = encodingFor(type, info.bits);
            info.floatingPoint = encoding == Encoding::bigFloat32 || encoding == Encoding::bigFloat64;
            if (info.floatingPoint)
                info.bits = encoding == Encoding::bigFloat64 ? 64 : 32;
            haveCommon = true;

            if (!encoding.has_value())
            {
                error = AudioFileError::unsupported;
                return nullptr;
            }
        }
        else if (id == "SSND")
        {
            unsigned char bytes[8]{};
            if (file->readAt(body, bytes, sizeof bytes, error) < sizeof bytes)
            {
                error = AudioFileError::damaged;
                return nullptr;
            }

            const auto offset = readBig32(bytes);
            dataStart = body + 8 + offset;
            const auto available = fileSize > *dataStart ? fileSize - *dataStart : 0;
            const auto said = size >= 8 + offset ? size - 8 - offset : 0;

            // A file whose writing was stopped may say more than it holds, or nothing.
            dataSize = said == 0 || said > available ? available : said;

            if (*dataStart + *dataSize >= fileSize)
                break;
        }
        else if (!isLayoutChunk(id) && size <= biggestChunkKept && body + size <= fileSize)
        {
            AudioChunk chunk{id, std::string(static_cast<std::size_t>(size), '\0')};
            file->readAt(body, chunk.bytes.data(), chunk.bytes.size(), error);
            if (error)
                return nullptr;
            info.chunks.push_back(std::move(chunk));
        }

        position = body + size + (size % 2);
    }

    if (error)
        return nullptr;

    if (!haveCommon || !dataStart.has_value() || info.channels <= 0 || !(info.sampleRate > 0.0) ||
        info.sampleRate > 4294967295.0 || info.bits <= 0)
    {
        error = AudioFileError::damaged;
        return nullptr;
    }

    const auto frameBytes = static_cast<std::uint64_t>(bytesPerSample(*encoding)) * static_cast<std::uint64_t>(info.channels);
    const auto held = *dataSize / frameBytes;
    info.frames = commonFrames == 0 ? held : std::min(commonFrames, held);

    return std::make_unique<AiffReader>(std::move(file), path, std::move(info), *encoding, *dataStart);
}

std::unique_ptr<AudioFileWriter> createAiff(std::unique_ptr<RandomAccessFile> file, const std::filesystem::path& path,
                                            const AudioFileSettings& settings, std::error_code& error)
{
    const auto encoding = settings.floatingPoint ? (settings.bits == 64 ? Encoding::bigFloat64 : Encoding::bigFloat32)
                        : settings.bits == 8     ? Encoding::signed8
                        : settings.bits == 16    ? Encoding::big16
                        : settings.bits == 24    ? Encoding::big24
                                                 : Encoding::big32;

    auto writer = std::make_unique<AiffWriter>(std::move(file), path, settings, encoding);
    writer->writeHeader(error);
    if (error)
        return nullptr;
    return writer;
}

}
