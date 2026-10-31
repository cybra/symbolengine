#include "Bytes.h"
#include "ChannelOrder.h"
#include "Formats.h"
#include "Samples.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <optional>

// WAV, as Microsoft's RIFF describes it, with WAVE_FORMAT_EXTENSIBLE, and RF64 (EBU Tech 3306) and BW64 (ITU-R BS.2088)
// for files past 4 GB, whose ds64 chunk holds the sizes the 32-bit fields can't. A file is written with a JUNK chunk
// where a ds64 chunk fits, so it becomes RF64 in place when it passes 4 GB.
namespace symbolengine::detail
{

namespace
{
constexpr std::uint64_t biggestChunkKept = 64 * 1024 * 1024;
constexpr std::uint32_t unknownSize = 0xFFFFFFFF;

// The GUIDs WAVE_FORMAT_EXTENSIBLE names a sample format by: the format's tag in the first two bytes, then the
// rest of the standard subtype GUID, or of the Ambisonic B-format one.
constexpr std::array<unsigned char, 14> standardSubtype{0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xAA, 0, 0x38, 0x9B, 0x71};
constexpr std::array<unsigned char, 14> ambisonicSubtype{0, 0, 0x21, 0x07, 0xD3, 0x11, 0x86, 0x44, 0xC8, 0xC1, 0xCA, 0, 0, 0};

constexpr std::uint16_t pcmTag = 1;
constexpr std::uint16_t floatTag = 3;
constexpr std::uint16_t extensibleTag = 0xFFFE;

class WavReader final : public AudioFileReader
{
public:
    WavReader(std::unique_ptr<RandomAccessFile> f, const std::filesystem::path& p, AudioInfo info, Encoding e,
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

class WavWriter final : public AudioFileWriter
{
public:
    WavWriter(std::unique_ptr<RandomAccessFile> f, const std::filesystem::path& p, const AudioFileSettings& s, Encoding e)
        : file(std::move(f)), encoding(e)
    {
        written = s;
        path = p;
        frameBytes = static_cast<std::size_t>(bytesPerSample(encoding)) * static_cast<std::size_t>(written.channels);
    }

    ~WavWriter() override
    {
        std::error_code ignored;
        finish(ignored);
    }

    void writeHeader(std::error_code& error)
    {
        const auto channels = static_cast<std::uint32_t>(written.channels);
        const auto rate = static_cast<std::uint32_t>(written.sampleRate + 0.5);
        const auto bits = static_cast<std::uint32_t>(written.bits);
        const auto blockAlign = static_cast<std::uint32_t>(frameBytes);
        const auto mask = written.channelMask != 0 ? written.channelMask : usualMask(written.channels);
        const auto extensible = written.channels > 2 || written.channelMask != 0;
        const auto tag = written.floatingPoint ? floatTag : pcmTag;

        std::string header = "RIFF";
        addLittle32(header, 0);
        header += "WAVE";

        // Room for a ds64 chunk, should the file pass 4 GB.
        header += "JUNK";
        addLittle32(header, 28);
        header.append(28, '\0');

        header += "fmt ";
        addLittle32(header, extensible ? 40 : written.floatingPoint ? 18 : 16);
        addLittle16(header, extensible ? extensibleTag : tag);
        addLittle16(header, channels);
        addLittle32(header, rate);
        addLittle32(header, rate * blockAlign);
        addLittle16(header, blockAlign);
        addLittle16(header, bits);
        if (extensible)
        {
            addLittle16(header, 22);
            addLittle16(header, bits);
            addLittle32(header, mask);
            addLittle16(header, tag);
            header.append(reinterpret_cast<const char*>(standardSubtype.data()), standardSubtype.size());
        }
        else if (written.floatingPoint)
        {
            addLittle16(header, 0);
        }

        // Every format but whole numbers has a fact chunk, holding the length.
        if (written.floatingPoint)
        {
            header += "fact";
            addLittle32(header, 4);
            factAt = header.size();
            addLittle32(header, 0);
        }

        for (const auto& chunk : written.chunks)
        {
            if (chunk.id.size() != 4 || isLayoutChunk(chunk.id) || chunk.bytes.size() > unknownSize - 1)
                continue;
            header += chunk.id;
            addLittle32(header, static_cast<std::uint32_t>(chunk.bytes.size()));
            header += chunk.bytes;
            if (chunk.bytes.size() % 2 != 0)
                header += '\0';
        }

        header += "data";
        dataSizeAt = header.size();
        addLittle32(header, 0);
        dataStart = header.size();

        file->writeAt(0, header.data(), header.size(), error);

        // A file whose header couldn't be written is left as it is.
        if (error)
            finished = true;
    }

private:
    void writeFrames(const float* const* channels, std::size_t frames, std::error_code& error) override
    {
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

        if (error)
            return;

        std::string sizes;
        const auto big = end - 8 > unknownSize || dataBytes > unknownSize;

        if (big)
        {
            std::string start = "RF64";
            addLittle32(start, unknownSize);
            start += "WAVE";
            start += "ds64";
            addLittle32(start, 28);
            addLittle64(start, end - 8);
            addLittle64(start, dataBytes);
            addLittle64(start, count);
            addLittle32(start, 0);
            file->writeAt(0, start.data(), start.size(), error);
            addLittle32(sizes, unknownSize);
        }
        else
        {
            std::string riff;
            addLittle32(riff, static_cast<std::uint32_t>(end - 8));
            file->writeAt(4, riff.data(), riff.size(), error);
            addLittle32(sizes, static_cast<std::uint32_t>(dataBytes));
        }

        if (!error)
            file->writeAt(dataSizeAt, sizes.data(), sizes.size(), error);

        if (!error && factAt != 0)
        {
            std::string length;
            addLittle32(length, count > unknownSize ? unknownSize : static_cast<std::uint32_t>(count));
            file->writeAt(factAt, length.data(), length.size(), error);
        }
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
        if (!error)
            file->sync(error);

        std::error_code closing;
        file->close(closing);
        if (!error)
            error = closing;
    }

    std::unique_ptr<RandomAccessFile> file;
    Encoding encoding;
    std::size_t frameBytes = 1;
    std::uint64_t dataStart = 0, dataBytes = 0;
    std::size_t dataSizeAt = 0, factAt = 0;
    std::vector<unsigned char> scratch;
};

struct Format
{
    std::uint16_t tag = 0;
    int channels = 0;
    double rate = 0.0;
    int blockAlign = 0;
    int bits = 0;
    std::uint32_t mask = 0;
};

// The sample format a fmt chunk names, or the reason it can't be read.
std::optional<Encoding> encodingFor(const Format& format, std::error_code& error)
{
    if (format.channels <= 0 || format.channels > mostChannels || !(format.rate > 0.0) || format.bits <= 0)
    {
        error = AudioFileError::damaged;
        return std::nullopt;
    }

    const auto bytes = (format.bits + 7) / 8;
    std::optional<Encoding> encoding;

    if (format.tag == pcmTag)
        encoding = bytes == 1 ? Encoding::unsigned8 : bytes == 2 ? Encoding::little16
                 : bytes == 3 ? Encoding::little24 : bytes == 4 ? Encoding::little32 : std::optional<Encoding>();
    else if (format.tag == floatTag)
        encoding = format.bits == 32 ? Encoding::littleFloat32 : format.bits == 64 ? Encoding::littleFloat64
                                                                                   : std::optional<Encoding>();

    if (!encoding.has_value())
    {
        error = AudioFileError::unsupported;
        return std::nullopt;
    }

    if (format.blockAlign != bytesPerSample(*encoding) * format.channels)
    {
        error = AudioFileError::damaged;
        return std::nullopt;
    }

    return encoding;
}
}

std::unique_ptr<AudioFileReader> openWav(std::unique_ptr<RandomAccessFile> file, const std::filesystem::path& path,
                                         std::error_code& error)
{
    const auto fileSize = file->size(error);
    if (error)
        return nullptr;

    unsigned char start[12]{};
    file->readAt(0, start, sizeof start, error);
    if (error)
        return nullptr;

    const auto sixtyFour = std::memcmp(start, "RIFF", 4) != 0;

    AudioInfo info;
    info.format = "WAV";
    std::optional<Format> format;
    std::optional<std::uint64_t> dataStart, dataSize;
    std::uint64_t ds64Data = 0;
    std::map<std::string, std::uint64_t> ds64Sizes;
    std::uint64_t kept = 0;

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
        std::uint64_t size = readLittle32(header + 4);

        if (id == "ds64" && sixtyFour)
        {
            std::string sizes(static_cast<std::size_t>(std::min<std::uint64_t>(size, 1024)), '\0');
            file->readAt(body, sizes.data(), sizes.size(), error);
            if (sizes.size() >= 28)
            {
                const auto* p = bytesOf(sizes);
                ds64Data = readLittle64(p + 8);
                const auto entries = readLittle32(p + 24);
                for (std::uint32_t i = 0; i < entries && 28 + (i + 1) * 12 <= sizes.size(); ++i)
                    ds64Sizes[std::string(sizes.data() + 28 + i * 12, 4)] = readLittle64(p + 28 + i * 12 + 4);
            }
        }

        if (sixtyFour && size == unknownSize)
            size = id == "data" ? ds64Data : ds64Sizes.contains(id) ? ds64Sizes[id] : 0;

        if (id == "fmt ")
        {
            unsigned char bytes[40]{};
            const auto got = file->readAt(body, bytes, static_cast<std::size_t>(std::min<std::uint64_t>(size, 40)), error);
            if (got < 16)
            {
                error = AudioFileError::damaged;
                return nullptr;
            }

            Format f;
            f.tag = readLittle16(bytes);
            f.channels = readLittle16(bytes + 2);
            f.rate = readLittle32(bytes + 4);
            f.blockAlign = readLittle16(bytes + 12);
            f.bits = readLittle16(bytes + 14);

            if (f.tag == extensibleTag)
            {
                if (got < 40)
                {
                    error = AudioFileError::damaged;
                    return nullptr;
                }

                f.mask = readLittle32(bytes + 20);
                const auto* subtype = bytes + 24;
                const auto known = std::equal(standardSubtype.begin(), standardSubtype.end(), subtype + 2) ||
                                   std::equal(ambisonicSubtype.begin(), ambisonicSubtype.end(), subtype + 2);
                f.tag = known ? readLittle16(subtype) : 0;
            }

            format = f;
        }
        else if (id == "data")
        {
            dataStart = body;
            const auto available = fileSize - body;

            // A file whose writing was stopped, or a stream's, may say more than it holds, or nothing.
            dataSize = size == 0 || size > available || (!sixtyFour && size == unknownSize) ? available : size;

            if (body + *dataSize >= fileSize)
                break;
            size = *dataSize;
        }
        else if (!isLayoutChunk(id) && size <= biggestChunkKept && body + size <= fileSize &&
                 kept + size <= mostKeptInAll)
        {
            kept += size;
            AudioChunk chunk{id, std::string(static_cast<std::size_t>(size), '\0')};
            file->readAt(body, chunk.bytes.data(), chunk.bytes.size(), error);
            if (error)
                return nullptr;
            info.chunks.push_back(std::move(chunk));
        }

        // A chunk saying more than the file holds is its last, and a ds64 chunk can say any size at all.
        if (size > fileSize - body)
            break;

        // A chunk of odd length is followed by a pad byte, which some writers leave out.
        auto next = body + size + (size % 2);
        if (size % 2 != 0 && next + 4 <= fileSize)
        {
            unsigned char padded[4]{}, unpadded[4]{};
            file->readAt(next, padded, 4, error);
            file->readAt(next - 1, unpadded, 4, error);
            if (!looksLikeId(padded) && looksLikeId(unpadded))
                --next;
        }
        position = next;
    }

    if (error)
        return nullptr;

    if (!format.has_value() || !dataStart.has_value())
    {
        error = AudioFileError::damaged;
        return nullptr;
    }

    const auto encoding = encodingFor(*format, error);
    if (!encoding.has_value())
        return nullptr;

    info.sampleRate = format->rate;
    info.channels = format->channels;
    info.bits = bytesPerSample(*encoding) * 8;
    info.floatingPoint = format->tag == floatTag;
    info.channelMask = format->mask;
    info.frames = *dataSize / static_cast<std::uint64_t>(format->blockAlign);

    return std::make_unique<WavReader>(std::move(file), path, std::move(info), *encoding, *dataStart);
}

std::unique_ptr<AudioFileWriter> createWav(std::unique_ptr<RandomAccessFile> file, const std::filesystem::path& path,
                                           const AudioFileSettings& settings, std::error_code& error)
{
    const auto encoding = settings.floatingPoint ? (settings.bits == 64 ? Encoding::littleFloat64 : Encoding::littleFloat32)
                        : settings.bits == 8     ? Encoding::unsigned8
                        : settings.bits == 16    ? Encoding::little16
                        : settings.bits == 24    ? Encoding::little24
                                                 : Encoding::little32;

    auto writer = std::make_unique<WavWriter>(std::move(file), path, settings, encoding);
    writer->writeHeader(error);
    if (error)
        return nullptr;
    return writer;
}

}
