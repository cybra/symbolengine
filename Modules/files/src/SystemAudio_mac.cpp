#include "ChannelOrder.h"
#include "FileStream.h"
#include "Formats.h"

#include <AudioToolbox/AudioToolbox.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <string>

// MPEG-4 audio, CAF, ADTS, and WAV and AIFF-C coded in ways of their own, through Apple's AudioToolbox: ExtAudioFile
// decodes AAC, ALAC, ADPCM and the rest, leaving out AAC's priming samples where the file says how many there are, and
// gives each channel in WAV's order.
namespace symbolengine::detail
{

namespace
{
OSStatus readCallback(void* source, SInt64 position, UInt32 count, void* bytes, UInt32* got)
{
    auto& stream = *static_cast<FileStream*>(source);
    stream.position = static_cast<std::uint64_t>(position);
    *got = static_cast<UInt32>(stream.read(bytes, count));
    return stream.error ? static_cast<OSStatus>(kAudioFileUnspecifiedError) : static_cast<OSStatus>(noErr);
}

SInt64 sizeCallback(void* source)
{
    return static_cast<SInt64>(static_cast<FileStream*>(source)->size);
}

std::string text(CFStringRef string)
{
    if (string == nullptr)
        return {};
    const auto most = CFStringGetMaximumSizeForEncoding(CFStringGetLength(string), kCFStringEncodingUTF8) + 1;
    std::string bytes(static_cast<std::size_t>(most), '\0');
    if (!CFStringGetCString(string, bytes.data(), most, kCFStringEncodingUTF8))
        return {};
    bytes.resize(std::strlen(bytes.c_str()));
    return bytes;
}

std::string typeName(AudioFileID file)
{
    AudioFileTypeID type = 0;
    auto size = static_cast<UInt32>(sizeof type);
    if (AudioFileGetProperty(file, kAudioFilePropertyFileFormat, &size, &type) != noErr)
        return {};

    CFStringRef name = nullptr;
    size = sizeof name;
    if (AudioFileGetGlobalInfo(kAudioFileGlobalInfo_FileTypeName, sizeof type, &type, &size, &name) != noErr || name == nullptr)
        return {};
    auto named = text(name);
    CFRelease(name);
    return named;
}

class SystemReader final : public AudioFileReader
{
public:
    SystemReader(std::unique_ptr<RandomAccessFile> f, const std::filesystem::path& p, SystemFormat kind) : file(std::move(f))
    {
        path = p;
        stream.file = file.get();
        switch (kind)
        {
            case SystemFormat::mpeg4: information.format = "M4A"; hint = kAudioFileM4AType; break;
            case SystemFormat::caf: information.format = "CAF"; hint = kAudioFileCAFType; break;
            case SystemFormat::adts: information.format = "AAC"; hint = kAudioFileAAC_ADTSType; break;
            case SystemFormat::wav: information.format = "WAV"; hint = kAudioFileWAVEType; break;
            case SystemFormat::aiff: information.format = "AIFF"; hint = kAudioFileAIFCType; break;
            case SystemFormat::asf:
            case SystemFormat::other: break;
        }
    }

    ~SystemReader() override
    {
        if (audio != nullptr)
            ExtAudioFileDispose(audio);
        if (opened != nullptr)
            AudioFileClose(opened);
    }

    void start(std::error_code& error)
    {
        stream.size = file->size(error);
        if (error)
            return;

        AudioStreamBasicDescription format{};
        SInt64 length = 0;
        auto size = static_cast<UInt32>(sizeof format);
        auto lengthSize = static_cast<UInt32>(sizeof length);

        if (AudioFileOpenWithCallbacks(&stream, &readCallback, nullptr, &sizeCallback, nullptr, hint, &opened) != noErr ||
            ExtAudioFileWrapAudioFileID(opened, false, &audio) != noErr ||
            ExtAudioFileGetProperty(audio, kExtAudioFileProperty_FileDataFormat, &size, &format) != noErr ||
            ExtAudioFileGetProperty(audio, kExtAudioFileProperty_FileLengthFrames, &lengthSize, &length) != noErr ||
            format.mChannelsPerFrame == 0 || !(format.mSampleRate > 0.0))
        {
            error = stream.error ? stream.error : make_error_code(AudioFileError::unsupported);
            return;
        }

        // A file AudioToolbox told the kind of is named as AudioToolbox names that kind.
        if (information.format.empty())
            information.format = typeName(opened);

        information.sampleRate = format.mSampleRate;
        information.channels = static_cast<int>(format.mChannelsPerFrame);
        information.frames = static_cast<std::uint64_t>(std::max<SInt64>(length, 0));
        information.bits = static_cast<int>(format.mBitsPerChannel);
        if (format.mFormatID == kAudioFormatAppleLossless)
            information.bits = format.mFormatFlags == kAppleLosslessFormatFlag_16BitSourceData ? 16
                             : format.mFormatFlags == kAppleLosslessFormatFlag_20BitSourceData ? 20
                             : format.mFormatFlags == kAppleLosslessFormatFlag_24BitSourceData ? 24 : 32;
        information.floatingPoint = format.mFormatID == kAudioFormatLinearPCM && (format.mFormatFlags & kAudioFormatFlagIsFloat) != 0;

        AudioStreamBasicDescription client{};
        client.mSampleRate = format.mSampleRate;
        client.mFormatID = kAudioFormatLinearPCM;
        client.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagIsNonInterleaved;
        client.mBytesPerPacket = 4;
        client.mFramesPerPacket = 1;
        client.mBytesPerFrame = 4;
        client.mChannelsPerFrame = format.mChannelsPerFrame;
        client.mBitsPerChannel = 32;

        if (ExtAudioFileSetProperty(audio, kExtAudioFileProperty_ClientDataFormat, sizeof client, &client) != noErr)
        {
            error = make_error_code(AudioFileError::unsupported);
            return;
        }

        // Channels in WAV's order, by the speakers usually meant by their number.
        if (const auto mask = usualMask(information.channels); information.channels > 2 && mask != 0)
        {
            AudioChannelLayout layout{};
            layout.mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelBitmap;
            layout.mChannelBitmap = mask;
            ExtAudioFileSetProperty(audio, kExtAudioFileProperty_ClientChannelLayout, sizeof layout, &layout);
        }
    }

private:
    std::size_t readFrames(std::uint64_t from, float* const* channels, std::size_t frames, std::error_code& error) override
    {
        if (from != next)
        {
            if (ExtAudioFileSeek(audio, static_cast<SInt64>(from)) != noErr)
            {
                next = nowhere;
                error = stream.error ? stream.error : make_error_code(AudioFileError::damaged);
                return 0;
            }
            next = from;
        }

        const auto count = static_cast<std::size_t>(information.channels);
        buffers.resize(offsetof(AudioBufferList, mBuffers) + count * sizeof(AudioBuffer));
        auto* list = reinterpret_cast<AudioBufferList*>(buffers.data());
        list->mNumberBuffers = static_cast<UInt32>(count);

        std::size_t done = 0;
        while (done < frames)
        {
            auto now = static_cast<UInt32>(std::min<std::size_t>(frames - done, 65536));
            for (std::size_t c = 0; c < count; ++c)
            {
                list->mBuffers[c].mNumberChannels = 1;
                list->mBuffers[c].mDataByteSize = now * 4;
                list->mBuffers[c].mData = channels[c] + done;
            }

            if (ExtAudioFileRead(audio, &now, list) != noErr)
            {
                error = stream.error ? stream.error : make_error_code(AudioFileError::damaged);
                break;
            }
            if (now == 0)
                break;
            done += now;
        }

        next = from + done;
        return done;
    }

    std::unique_ptr<RandomAccessFile> file;
    FileStream stream;
    AudioFileTypeID hint = 0;
    AudioFileID opened = nullptr;
    ExtAudioFileRef audio = nullptr;
    std::vector<std::byte> buffers;
    std::uint64_t next = 0;
};
}

std::vector<std::string> systemExtensions()
{
    CFArrayRef found = nullptr;
    auto size = static_cast<UInt32>(sizeof found);
    if (AudioFileGetGlobalInfo(kAudioFileGlobalInfo_AllExtensions, 0, nullptr, &size, &found) != noErr || found == nullptr)
        return {};

    std::vector<std::string> extensions;
    for (CFIndex i = 0; i < CFArrayGetCount(found); ++i)
    {
        auto extension = "." + text(static_cast<CFStringRef>(CFArrayGetValueAtIndex(found, i)));
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (extension.size() > 1 && std::find(extensions.begin(), extensions.end(), extension) == extensions.end())
            extensions.push_back(std::move(extension));
    }
    CFRelease(found);
    return extensions;
}

std::unique_ptr<AudioFileReader> openSystemAudio(std::unique_ptr<RandomAccessFile> file, const std::filesystem::path& path,
                                                 SystemFormat format, std::error_code& error)
{
    auto reader = std::make_unique<SystemReader>(std::move(file), path, format);
    reader->start(error);
    if (error)
        return nullptr;
    return reader;
}

}
