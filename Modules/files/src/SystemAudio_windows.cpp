#include "Formats.h"
#include "Mp4.h"

#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
    #define NOMINMAX
#endif
#include <windows.h>

#include <combaseapi.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <algorithm>
#include <cmath>

// MPEG-4 audio, ADTS and Windows Media's ASF through Media Foundation's source reader, which decodes to floats with the channels in WAV's
// order; Windows has no reader for CAF, nor for WAV and AIFF-C coded in ways of their own. Media Foundation acts on an
// MPEG-4 file's edit list, giving the encoder's priming the time of the audio after it, but times a file that gives
// its priming in iTunSMPB instead, as Apple's encoder writes it, from its first sample, priming and all; for that file
// the decoder's samples are counted from where iTunSMPB says the audio starts. The first stretch decoded after a jump carries the time it starts at, which
// places it and those after it, so a read starts at exactly the sample asked for; a jump starts half a second before
// that sample, so the decoder settles first.
//
// Media Foundation is loaded when a file first needs it, so a Windows without it, as its N editions are, reads these
// files as unsupported and runs the rest. Threads the caller hasn't started COM on use the process's multithreaded
// apartment, kept open for them, so no thread's COM is started or changed here.
namespace symbolengine::detail
{

namespace
{
// A COM object, released as it goes.
template <typename Interface>
struct Com
{
    Interface* object = nullptr;

    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    ~Com() { reset(); }

    void reset()
    {
        if (object != nullptr)
            object->Release();
        object = nullptr;
    }

    Interface** put()
    {
        reset();
        return &object;
    }

    Interface* operator->() const { return object; }
};

template <typename Function>
Function find(HMODULE library, const char* name)
{
    return library == nullptr ? nullptr : reinterpret_cast<Function>(reinterpret_cast<void (*)()>(GetProcAddress(library, name)));
}

struct MediaFoundation
{
    decltype(&MFStartup) startup = nullptr;
    decltype(&MFCreateFile) createFile = nullptr;
    decltype(&MFCreateMediaType) createMediaType = nullptr;
    decltype(&MFCreateSourceReaderFromByteStream) createReader = nullptr;
    bool ready = false;
};

const MediaFoundation& mediaFoundation()
{
    static const MediaFoundation loaded = []
    {
        MediaFoundation found;
        CO_MTA_USAGE_COOKIE cookie{};
        if (FAILED(CoIncrementMTAUsage(&cookie)))
            return found;

        const auto platform = LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        const auto reading = LoadLibraryExW(L"mfreadwrite.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        found.startup = find<decltype(&MFStartup)>(platform, "MFStartup");
        found.createFile = find<decltype(&MFCreateFile)>(platform, "MFCreateFile");
        found.createMediaType = find<decltype(&MFCreateMediaType)>(platform, "MFCreateMediaType");
        found.createReader = find<decltype(&MFCreateSourceReaderFromByteStream)>(reading, "MFCreateSourceReaderFromByteStream");
        found.ready = found.startup != nullptr && found.createFile != nullptr && found.createMediaType != nullptr &&
                      found.createReader != nullptr && SUCCEEDED(found.startup(MF_VERSION, MFSTARTUP_LITE));
        return found;
    }();
    return loaded;
}

constexpr auto audioStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM);

// Media Foundation's names for Apple Lossless and plain PCM, each a WAV format tag in the GUID audio subtypes share.
constexpr GUID audioSubtype(unsigned long tag) { return {tag, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}}; }
constexpr GUID appleLossless = audioSubtype(0x6C61);
constexpr GUID pcm = audioSubtype(0x0001);

class SystemReader final : public AudioFileReader
{
public:
    SystemReader(std::unique_ptr<RandomAccessFile> f, const std::filesystem::path& p, SystemFormat kind)
        : file(std::move(f)), adts(kind == SystemFormat::adts), asf(kind == SystemFormat::asf)
    {
        path = p;
        information.format = adts ? "AAC" : asf ? "WMA" : "M4A";
    }

    void start(std::error_code& error)
    {
        const auto& found = mediaFoundation();
        Com<IMFByteStream> bytes;
        Com<IMFAttributes> described;

        // The kind of file is given, so it reads whatever its name ends in.
        if (!found.ready ||
            FAILED(found.createFile(MF_ACCESSMODE_READ, MF_OPENMODE_FAIL_IF_NOT_EXIST, MF_FILEFLAGS_NONE, path.c_str(), bytes.put())) ||
            FAILED(bytes->QueryInterface(IID_PPV_ARGS(described.put()))) ||
            FAILED(described->SetString(MF_BYTESTREAM_CONTENT_TYPE, adts ? L"audio/aac" : asf ? L"video/x-ms-asf" : L"audio/mp4")) ||
            FAILED(found.createReader(bytes.object, nullptr, reader.put())))
        {
            error = AudioFileError::unsupported;
            return;
        }

        Com<IMFMediaType> wanted, current;
        UINT32 channels = 0, rate = 0;
        if (FAILED(reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE)) ||
            FAILED(reader->SetStreamSelection(audioStream, TRUE)) || FAILED(found.createMediaType(wanted.put())) ||
            FAILED(wanted->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio)) ||
            FAILED(wanted->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float)) ||
            FAILED(reader->SetCurrentMediaType(audioStream, nullptr, wanted.object)) ||
            FAILED(reader->GetCurrentMediaType(audioStream, current.put())) ||
            FAILED(current->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels)) ||
            FAILED(current->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate)) || channels == 0 || rate == 0)
        {
            error = AudioFileError::unsupported;
            return;
        }

        information.sampleRate = rate;
        information.channels = static_cast<int>(channels);

        // The bits a lossless file keeps.
        Com<IMFMediaType> native;
        GUID stored{};
        UINT32 bits = 0;
        if (SUCCEEDED(reader->GetNativeMediaType(audioStream, 0, native.put())) && SUCCEEDED(native->GetGUID(MF_MT_SUBTYPE, &stored)) &&
            (stored == appleLossless || stored == pcm) && SUCCEEDED(native->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits)))
            information.bits = static_cast<int>(bits);

        PROPVARIANT duration;
        PropVariantInit(&duration);
        if (SUCCEEDED(reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &duration)))
            information.frames = static_cast<std::uint64_t>(std::llround(static_cast<double>(duration.uhVal.QuadPart) * rate / 1.0e7));
        PropVariantClear(&duration);

        if (!adts && !asf)
        {
            const auto told = readMp4Audio(*file, rate, error);
            if (error)
                return;
            if (!told.edited)
                priming = told.priming;
            information.frames = told.frames ? *told.frames : information.frames - std::min(information.frames, priming);
            if (information.bits == 0)
                information.bits = told.losslessBits;
        }
    }

private:
    std::size_t heldFrames() const { return held.size() / static_cast<std::size_t>(information.channels); }

    // Moves to a little before a frame, from where decoding reaches it settled.
    bool seek(std::uint64_t frame)
    {
        const auto settle = static_cast<std::uint64_t>(information.sampleRate / 2);
        const auto decoded = frame + priming;
        const auto start = decoded > settle ? decoded - settle : 0;

        PROPVARIANT position;
        PropVariantInit(&position);
        position.vt = VT_I8;
        position.hVal.QuadPart = std::llround(static_cast<double>(start) * 1.0e7 / information.sampleRate);
        const auto moved = SUCCEEDED(reader->SetCurrentPosition(GUID_NULL, position));
        PropVariantClear(&position);

        held.clear();
        heldStart = 0;
        placed = false;
        ended = false;
        waiting = false;
        pending.clear();
        return moved;
    }

    // Places a decoded stretch that starts at the frame given: the first after a jump by that frame, letting go of what
    // lies before the audio, a stretch or more of the encoder's priming, and each after it following on.
    void place(const std::vector<float>& stretch, long long first)
    {
        const auto channelCount = static_cast<std::size_t>(information.channels);
        std::size_t skip = 0;
        if (!placed)
        {
            const auto before = first < 0 ? static_cast<std::size_t>(-first) : std::size_t{0};
            if (before * channelCount >= stretch.size())
                return;
            skip = before * channelCount;
            heldStart = static_cast<std::uint64_t>(first + static_cast<long long>(before));
            placed = true;
        }
        held.insert(held.end(), stretch.begin() + static_cast<std::ptrdiff_t>(skip), stretch.end());
    }

    // Decodes on until a stretch is placed; false at the end or on a problem. Each stretch waits for the next: Media
    // Foundation gives the priming of a file with an edit list the time of the audio after it, so before the audio is
    // placed, a stretch the next one starts no later than is that priming, and is let go.
    bool decodeMore(std::error_code& error)
    {
        const auto before = held.size();
        while (held.size() == before)
        {
            if (ended)
            {
                if (!waiting)
                    return false;
                place(pending, pendingFirst);
                waiting = false;
                continue;
            }

            DWORD flags = 0;
            LONGLONG time = 0;
            Com<IMFSample> sample;
            if (FAILED(reader->ReadSample(audioStream, 0, nullptr, &flags, &time, sample.put())))
            {
                error = AudioFileError::damaged;
                return false;
            }

            ended = (flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0;
            if (sample.object == nullptr)
                continue;

            Com<IMFMediaBuffer> buffer;
            BYTE* bytes = nullptr;
            DWORD length = 0;
            if (FAILED(sample->ConvertToContiguousBuffer(buffer.put())) || FAILED(buffer->Lock(&bytes, nullptr, &length)))
            {
                error = AudioFileError::damaged;
                return false;
            }
            const auto* samples = reinterpret_cast<const float*>(bytes);
            std::vector<float> next(samples, samples + static_cast<std::size_t>(length) / sizeof(float));
            buffer->Unlock();

            const auto first = std::llround(static_cast<double>(time) * information.sampleRate / 1.0e7) - static_cast<long long>(priming);
            if (waiting && (placed || first > pendingFirst))
                place(pending, pendingFirst);
            pending = std::move(next);
            pendingFirst = first;
            waiting = true;
        }
        return true;
    }

    std::size_t readFrames(std::uint64_t from, float* const* channels, std::size_t frames, std::error_code& error) override
    {
        // Reading on from where the last read ended keeps what is decoded; anywhere else, decoding starts again.
        if (from < heldStart || from > heldStart + heldFrames())
        {
            if (!seek(from))
            {
                error = AudioFileError::damaged;
                return 0;
            }
        }

        const auto count = static_cast<std::size_t>(information.channels);
        std::size_t done = 0;
        while (done < frames)
        {
            const auto at = from + done;
            if (at >= heldStart && at < heldStart + heldFrames())
            {
                const auto offset = static_cast<std::size_t>(at - heldStart);
                const auto now = std::min(frames - done, heldFrames() - offset);
                for (std::size_t c = 0; c < count; ++c)
                    for (std::size_t i = 0; i < now; ++i)
                        channels[c][done + i] = held[(offset + i) * count + c];
                done += now;
                continue;
            }

            // Decoding that starts after the place asked for can't reach it.
            if (placed && at < heldStart)
            {
                error = AudioFileError::damaged;
                break;
            }

            // What lies before the place asked for is let go, then more is decoded.
            heldStart += heldFrames();
            held.clear();
            if (!decodeMore(error))
                break;
        }

        // What has been read is let go, keeping the rest for the next read.
        if (const auto end = from + done; end > heldStart && end <= heldStart + heldFrames())
        {
            held.erase(held.begin(), held.begin() + static_cast<std::ptrdiff_t>((end - heldStart) * count));
            heldStart = end;
        }
        return done;
    }

    std::unique_ptr<RandomAccessFile> file;
    bool adts = false;
    bool asf = false;
    std::uint64_t priming = 0;    // what Media Foundation's times count that the audio doesn't: iTunSMPB's priming
    Com<IMFSourceReader> reader;
    std::vector<float> held;
    std::uint64_t heldStart = 0;
    bool placed = false;
    bool ended = false;
    std::vector<float> pending;
    long long pendingFirst = 0;
    bool waiting = false;
};
}

std::vector<std::string> systemExtensions()
{
    return {".m4a", ".m4b", ".mp4", ".aac", ".wma", ".asf", ".wm"};
}

std::unique_ptr<AudioFileReader> openSystemAudio(std::unique_ptr<RandomAccessFile> file, const std::filesystem::path& path,
                                                 SystemFormat format, std::error_code& error)
{
    if (format != SystemFormat::mpeg4 && format != SystemFormat::adts && format != SystemFormat::asf)
    {
        error = AudioFileError::unsupported;
        return nullptr;
    }

    auto reader = std::make_unique<SystemReader>(std::move(file), path, format);
    reader->start(error);
    if (error)
        return nullptr;
    return reader;
}

}
