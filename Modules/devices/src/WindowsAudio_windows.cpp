#include "Systems.h"

#if defined(SYMBOLENGINE_ASIO)
    #include "AsioSystem.h"
#endif

#include <symbolengine/devices/AudioSystem.h>

#ifndef NOMINMAX
    #define NOMINMAX
#endif
#include <windows.h>

// The property keys and interface identifiers below defined here, once.
#include <initguid.h>

// mmdeviceapi.h declares the property keys' macro, which functiondiscoverykeys_devpkey.h uses, so it comes first.
#include <mmdeviceapi.h>

#include <audioclient.h>
#include <audiopolicy.h>
#include <avrt.h>
#include <functiondiscoverykeys_devpkey.h>
#include <powrprof.h>

// MinGW's headers lack the suspend and resume notification Windows 8 added; Microsoft's have it.
#if defined(__MINGW32__) && !defined(DEVICE_NOTIFY_CALLBACK)
    #define DEVICE_NOTIFY_CALLBACK 2
typedef ULONG CALLBACK DEVICE_NOTIFY_CALLBACK_ROUTINE(PVOID context, ULONG type, PVOID setting);
typedef struct
{
    DEVICE_NOTIFY_CALLBACK_ROUTINE* Callback;
    PVOID Context;
} DEVICE_NOTIFY_SUBSCRIBE_PARAMETERS;
extern "C" DWORD WINAPI PowerRegisterSuspendResumeNotification(DWORD flags, HANDLE recipient, PHPOWERNOTIFY handle);
extern "C" DWORD WINAPI PowerUnregisterSuspendResumeNotification(HPOWERNOTIFY handle);
#endif

// COM's interfaces have no virtual destructors, by design; the objects here delete themselves in Release.
#if defined(__GNUC__)
    #pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

// Windows Audio behind the devices module's system layer. Each endpoint is one direction, so a device here is an
// input or an output, never both. The devices module calls only from threads of its own, which COM puts in the
// process's multithreaded apartment once it is kept alive with CoIncrementMTAUsage, so no thread's COM is started or
// changed. Streams are driven by the system's events. A shared-mode input is given a buffer as long as its thread may
// be held up, apart from the period it delivers, and each wake takes every packet waiting with its flags and its
// device position, so a discontinuity becomes the gap its position shows. Nothing is converted to another rate: a
// shared-mode stream runs at the system's mix rate, and exclusive mode at a format the device takes, or fails.
namespace symbolengine
{

namespace
{
template <typename T>
class Com
{
public:
    Com() = default;
    ~Com() { reset(); }
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    Com(Com&& other) noexcept : pointer(std::exchange(other.pointer, nullptr)) {}
    Com& operator=(Com&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            pointer = std::exchange(other.pointer, nullptr);
        }
        return *this;
    }

    T* operator->() const { return pointer; }
    T* get() const { return pointer; }
    T** put()
    {
        reset();
        return &pointer;
    }
    explicit operator bool() const { return pointer != nullptr; }

    void reset()
    {
        if (pointer != nullptr)
            pointer->Release();
        pointer = nullptr;
    }

private:
    T* pointer = nullptr;
};

std::string utf8(const wchar_t* text)
{
    if (text == nullptr)
        return {};
    const auto size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1)
        return {};
    std::string result(static_cast<std::size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring wide(const std::string& text)
{
    if (text.empty())
        return {};
    const auto size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    std::wstring result(static_cast<std::size_t>(std::max(size - 1, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, result.data(), size);
    return result;
}

// COM in the process's multithreaded apartment for every thread that hasn't one of its own, for as long as the
// process runs.
void keepComAlive()
{
    static const bool kept = [] {
        CO_MTA_USAGE_COOKIE cookie{};
        return SUCCEEDED(CoIncrementMTAUsage(&cookie));
    }();
    (void)kept;
}

Com<IMMDeviceEnumerator> enumerator()
{
    keepComAlive();
    Com<IMMDeviceEnumerator> made;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                     reinterpret_cast<void**>(made.put()));
    return made;
}

std::string idOf(IMMDevice* device)
{
    LPWSTR id = nullptr;
    if (FAILED(device->GetId(&id)) || id == nullptr)
        return {};
    auto result = utf8(id);
    CoTaskMemFree(id);
    return result;
}

std::string textProperty(IPropertyStore* store, const PROPERTYKEY& key)
{
    PROPVARIANT value;
    PropVariantInit(&value);
    std::string result;
    if (SUCCEEDED(store->GetValue(key, &value)) && value.vt == VT_LPWSTR)
        result = utf8(value.pwszVal);
    PropVariantClear(&value);
    return result;
}

// The shared-mode format the user chose for the device in Sound settings, read without making a client: copied
// only as far as it says it reaches.
bool deviceFormat(IPropertyStore* store, WAVEFORMATEXTENSIBLE& format)
{
    PROPVARIANT value;
    PropVariantInit(&value);
    bool found = false;
    if (SUCCEEDED(store->GetValue(PKEY_AudioEngine_DeviceFormat, &value)) && value.vt == VT_BLOB &&
        value.blob.cbSize >= sizeof(WAVEFORMATEX))
    {
        std::memset(&format, 0, sizeof(format));
        std::memcpy(&format, value.blob.pBlobData, std::min<std::size_t>(value.blob.cbSize, sizeof(format)));
        found = true;
    }
    PropVariantClear(&value);
    return found;
}

AudioLoss lossOf(AudioSessionDisconnectReason reason)
{
    switch (reason)
    {
        case DisconnectReasonDeviceRemoval: return AudioLoss::unplugged;
        case DisconnectReasonExclusiveModeOverride: return AudioLoss::takenExclusively;
        case DisconnectReasonServerShutdown: return AudioLoss::serviceRestarted;
        default: return AudioLoss::other;
    }
}

// Samples as Windows Audio holds them, to and from floats.
enum class Sample
{
    float32,
    int16,
    int24,      // packed in three bytes
    int24in32,  // the top three bytes of four
    int32
};

bool sampleOf(const WAVEFORMATEXTENSIBLE& format, Sample& sample)
{
    const auto& base = format.Format;
    const bool extensible = base.wFormatTag == WAVE_FORMAT_EXTENSIBLE && base.cbSize >= 22;
    const bool isFloat = base.wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                         (extensible && IsEqualGUID(format.SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT));
    const bool isPcm =
        base.wFormatTag == WAVE_FORMAT_PCM || (extensible && IsEqualGUID(format.SubFormat, KSDATAFORMAT_SUBTYPE_PCM));
    if (isFloat && base.wBitsPerSample == 32)
        sample = Sample::float32;
    else if (isPcm && base.wBitsPerSample == 16)
        sample = Sample::int16;
    else if (isPcm && base.wBitsPerSample == 24)
        sample = Sample::int24;
    else if (isPcm && base.wBitsPerSample == 32)
        sample = extensible && format.Samples.wValidBitsPerSample == 24 ? Sample::int24in32 : Sample::int32;
    else
        return false;
    return true;
}

float toFloat(const BYTE* at, Sample sample)
{
    switch (sample)
    {
        case Sample::float32:
        {
            float value;
            std::memcpy(&value, at, 4);
            return value;
        }
        case Sample::int16:
        {
            std::int16_t value;
            std::memcpy(&value, at, 2);
            return static_cast<float>(value) / 32768.0f;
        }
        case Sample::int24:
        {
            const auto value = static_cast<std::int32_t>(static_cast<std::uint32_t>(at[0]) << 8 |
                                                         static_cast<std::uint32_t>(at[1]) << 16 |
                                                         static_cast<std::uint32_t>(at[2]) << 24) >> 8;
            return static_cast<float>(value) / 8388608.0f;
        }
        case Sample::int24in32:
        case Sample::int32:
        {
            std::int32_t value;
            std::memcpy(&value, at, 4);
            return static_cast<float>(static_cast<double>(value) / 2147483648.0);
        }
    }
    return 0.0f;
}

void fromFloat(float value, BYTE* at, Sample sample)
{
    const auto held = std::isfinite(value) ? std::clamp(value, -1.0f, 1.0f) : 0.0f;
    switch (sample)
    {
        case Sample::float32: std::memcpy(at, &held, 4); return;
        case Sample::int16:
        {
            const auto whole = static_cast<std::int16_t>(std::clamp(std::lround(held * 32768.0f), -32768L, 32767L));
            std::memcpy(at, &whole, 2);
            return;
        }
        case Sample::int24:
        {
            const auto whole = static_cast<std::int32_t>(std::clamp(std::lround(held * 8388608.0f), -8388608L, 8388607L));
            at[0] = static_cast<BYTE>(whole & 0xff);
            at[1] = static_cast<BYTE>((whole >> 8) & 0xff);
            at[2] = static_cast<BYTE>((whole >> 16) & 0xff);
            return;
        }
        case Sample::int24in32:
        {
            const auto whole = static_cast<std::int32_t>(std::clamp(std::lround(held * 8388608.0f), -8388608L, 8388607L));
            const auto shifted = static_cast<std::int32_t>(static_cast<std::uint32_t>(whole) << 8);
            std::memcpy(at, &shifted, 4);
            return;
        }
        case Sample::int32:
        {
            const auto whole = static_cast<std::int32_t>(
                std::clamp(std::llround(static_cast<double>(held) * 2147483648.0), -2147483648LL, 2147483647LL));
            std::memcpy(at, &whole, 4);
            return;
        }
    }
}

// What the system says of a session, passed to the stream's client: removal, another program's exclusive use, the
// audio service stopping, or its format changing in Sound settings.
class SessionEvents final : public IAudioSessionEvents
{
public:
    std::atomic<AudioSystemClient*> client{nullptr};

    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(++references); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const auto left = --references;
        if (left == 0)
            delete this;
        return static_cast<ULONG>(left);
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override
    {
        if (IsEqualIID(id, __uuidof(IUnknown)) || IsEqualIID(id, __uuidof(IAudioSessionEvents)))
        {
            AddRef();
            *out = static_cast<IAudioSessionEvents*>(this);
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE OnSessionDisconnected(AudioSessionDisconnectReason reason) override
    {
        if (auto* to = client.load())
        {
            if (reason == DisconnectReasonFormatChanged)
                to->changed();
            else
                to->lost(lossOf(reason), static_cast<std::int64_t>(reason));
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnDisplayNameChanged(LPCWSTR, LPCGUID) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnIconPathChanged(LPCWSTR, LPCGUID) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnSimpleVolumeChanged(float, BOOL, LPCGUID) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnChannelVolumeChanged(DWORD, float*, DWORD, LPCGUID) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnGroupingParamChanged(LPCGUID, LPCGUID) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnStateChanged(AudioSessionState) override { return S_OK; }

private:
    std::atomic<long> references{1};
};

class WindowsAudioStream final : public AudioSystemStream
{
public:
    ~WindowsAudioStream() override
    {
        stop();
        if (session && events != nullptr)
            session->UnregisterAudioSessionNotification(events);
        if (events != nullptr)
            events->Release();
        if (wake != nullptr)
            CloseHandle(wake);
        if (quit != nullptr)
            CloseHandle(quit);
    }

    // Sets up a client on the device for the request; false with the error set when it can't.
    bool open(IMMDevice* device, bool capturing, const AudioStreamRequest& asked, AudioError& error)
    {
        capture = capturing;
        request = asked;
        const auto& wanted = capture ? request.inputChannels : request.outputChannels;
        if ((capture && !request.outputChannels.empty()) || (!capture && !request.inputChannels.empty()))
        {
            error = {capture ? "A Windows Audio input can't also play." : "A Windows Audio output can't also record.",
                     "open"};
            return false;
        }

        if (!activate(device, error))
            return false;

        // The format: the mix format in shared mode, which runs at the system's own rate; in exclusive mode the first
        // the device takes at the rate asked for.
        WAVEFORMATEX* mix = nullptr;
        if (FAILED(audioClient->GetMixFormat(&mix)) || mix == nullptr)
        {
            error = {"Windows Audio gave no format for the device.", "GetMixFormat"};
            return false;
        }
        std::memset(&format, 0, sizeof(format));
        std::memcpy(&format, mix, std::min<std::size_t>(sizeof(WAVEFORMATEX) + mix->cbSize, sizeof(format)));
        CoTaskMemFree(mix);
        channels = format.Format.nChannels;
        for (const auto channel : wanted)
            if (channel < 0 || channel >= channels)
            {
                error = {"The device doesn't have that channel.", "open"};
                return false;
            }

        if (request.exclusive)
        {
            if (!openExclusive(device, error))
                return false;
        }
        else
        {
            if (request.sampleRate > 0.0 && std::abs(request.sampleRate - format.Format.nSamplesPerSec) > 0.5)
            {
                error = {"Windows Audio's shared mode runs this device at " +
                             std::to_string(format.Format.nSamplesPerSec) +
                             " Hz, the rate set for it in Sound settings; exclusive mode runs it at others.",
                         "open"};
                return false;
            }
            if (!sampleOf(format, sample))
            {
                error = {"Windows Audio's format for the device isn't one SymbolEngine reads.", "open"};
                return false;
            }
            rawStream();
            // An input's buffer holds as long as its thread may be held up, apart from its period.
            const auto duration = capture ? static_cast<REFERENCE_TIME>(std::llround(request.inputBufferSeconds * 1e7)) : 0;
            const auto result = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                                   duration, 0, &format.Format, nullptr);
            if (FAILED(result))
            {
                error = {"Windows Audio didn't open the device.", "Initialize", result};
                return false;
            }
        }

        wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        quit = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        UINT32 frames = 0;
        if (wake == nullptr || quit == nullptr || FAILED(audioClient->SetEventHandle(wake)) ||
            FAILED(audioClient->GetBufferSize(&frames)))
        {
            error = {"Windows Audio didn't set up the device's buffer.", "SetEventHandle"};
            return false;
        }
        bufferFrames = frames;
        REFERENCE_TIME standard = 0, smallest = 0;
        audioClient->GetDevicePeriod(&standard, &smallest);
        const double rate = format.Format.nSamplesPerSec;
        periodFrames = request.exclusive ? bufferFrames
                                         : static_cast<UINT32>(std::max<long long>(1, std::llround(static_cast<double>(standard) * rate / 1e7)));

        const auto service = capture ? audioClient->GetService(__uuidof(IAudioCaptureClient),
                                                          reinterpret_cast<void**>(captureClient.put()))
                                     : audioClient->GetService(__uuidof(IAudioRenderClient),
                                                          reinterpret_cast<void**>(renderClient.put()));
        if (FAILED(service))
        {
            error = {"Windows Audio didn't give the device's buffer.", "GetService", service};
            return false;
        }
        if (SUCCEEDED(audioClient->GetService(__uuidof(IAudioSessionControl), reinterpret_cast<void**>(session.put()))))
        {
            events = new SessionEvents;
            session->RegisterAudioSessionNotification(events);
        }

        // One buffer of floats for each channel asked for, room for a whole device buffer.
        capacity = std::max<std::size_t>(bufferFrames, 4096);
        buffers.assign(wanted.size(), std::vector<float>(capacity, 0.0f));
        for (auto& buffer : buffers)
        {
            pointers.push_back(buffer.data());
            constPointers.push_back(buffer.data());
        }
        return true;
    }

    AudioStreamInfo info() const override
    {
        AudioStreamInfo now;
        now.sampleRate = format.Format.nSamplesPerSec;
        now.maxBlock = static_cast<int>(std::max(periodFrames, request.exclusive ? bufferFrames : periodFrames * 2));
        now.inputChannels = capture ? static_cast<int>(request.inputChannels.size()) : 0;
        now.outputChannels = capture ? 0 : static_cast<int>(request.outputChannels.size());
        REFERENCE_TIME latency = 0;
        audioClient->GetStreamLatency(&latency);
        const auto frames = static_cast<std::int64_t>(std::llround(static_cast<double>(latency) * now.sampleRate / 1e7)) + periodFrames;
        (capture ? now.inputLatency : now.outputLatency) = latency > 0 ? frames : -1;
        const auto given = scheduled.load();
        now.priority = given == 1 ? "Pro Audio, from the multimedia scheduler" : given == 0 ? "normal; Pro Audio refused" : "";
        return now;
    }

    bool start(AudioSystemClient& to, AudioError& error) override
    {
        client = &to;
        if (events != nullptr)
            events->client = &to;
        ResetEvent(quit);

        if (!capture)
        {
            // Silence for the room the device has, so it starts with nothing old in it.
            UINT32 padding = 0;
            audioClient->GetCurrentPadding(&padding);
            BYTE* data = nullptr;
            const auto room = request.exclusive ? bufferFrames : bufferFrames - padding;
            if (room > 0 && SUCCEEDED(renderClient->GetBuffer(room, &data)))
                renderClient->ReleaseBuffer(room, AUDCLNT_BUFFERFLAGS_SILENT);
        }

        const auto started = audioClient->Start();
        if (FAILED(started))
        {
            error = {"Windows Audio didn't start the device.", "Start", started};
            client = nullptr;
            return false;
        }
        running = true;
        thread = std::thread([this] { run(); });
        return true;
    }

    void stop() noexcept override
    {
        if (!running)
            return;
        running = false;
        SetEvent(quit);
        if (thread.joinable())
            thread.join();
        audioClient->Stop();
        audioClient->Reset();
        if (events != nullptr)
            events->client = nullptr;
        client = nullptr;
    }

private:
    bool activate(IMMDevice* device, AudioError& error)
    {
        const auto result = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                             reinterpret_cast<void**>(audioClient.put()));
        if (FAILED(result))
        {
            error = {result == E_ACCESSDENIED
                         ? "Windows didn't let the program use the device. Allow it in Settings, Privacy & security, Microphone."
                         : "Windows Audio didn't open the device.",
                     "Activate", result};
            return false;
        }
        return true;
    }

    // The device's own stream for recording, without the system's or its maker's effects, where it offers one.
    void rawStream()
    {
        if (!capture)
            return;
        Com<IAudioClient2> second;
        if (FAILED(audioClient->QueryInterface(__uuidof(IAudioClient2), reinterpret_cast<void**>(second.put()))))
            return;
        AudioClientProperties properties{};
        properties.cbSize = sizeof(properties);
        properties.bIsOffload = FALSE;
        properties.eCategory = AudioCategory_Media;
        properties.Options = AUDCLNT_STREAMOPTIONS_RAW;
        raw = SUCCEEDED(second->SetClientProperties(&properties));
    }

    bool openExclusive(IMMDevice* device, AudioError& error)
    {
        const auto rate = request.sampleRate > 0.0 ? static_cast<DWORD>(std::lround(request.sampleRate))
                                                   : format.Format.nSamplesPerSec;
        const auto mask = format.Format.wFormatTag == WAVE_FORMAT_EXTENSIBLE ? format.dwChannelMask : 0;
        struct Try
        {
            Sample sample;
            WORD bits, valid;
            GUID kind;
        };
        const Try tries[] = {{Sample::float32, 32, 32, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT},
                             {Sample::int32, 32, 32, KSDATAFORMAT_SUBTYPE_PCM},
                             {Sample::int24in32, 32, 24, KSDATAFORMAT_SUBTYPE_PCM},
                             {Sample::int24, 24, 24, KSDATAFORMAT_SUBTYPE_PCM},
                             {Sample::int16, 16, 16, KSDATAFORMAT_SUBTYPE_PCM}};

        for (const auto& candidate : tries)
        {
            WAVEFORMATEXTENSIBLE asking{};
            asking.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
            asking.Format.nChannels = static_cast<WORD>(channels);
            asking.Format.nSamplesPerSec = rate;
            asking.Format.wBitsPerSample = candidate.bits;
            asking.Format.nBlockAlign = static_cast<WORD>(channels * candidate.bits / 8);
            asking.Format.nAvgBytesPerSec = rate * asking.Format.nBlockAlign;
            asking.Format.cbSize = 22;
            asking.Samples.wValidBitsPerSample = candidate.valid;
            asking.dwChannelMask = mask;
            asking.SubFormat = candidate.kind;
            if (audioClient->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &asking.Format, nullptr) != S_OK)
                continue;

            REFERENCE_TIME standard = 0, smallest = 0;
            audioClient->GetDevicePeriod(&standard, &smallest);
            auto duration = request.blockSize > 0
                                ? std::max<REFERENCE_TIME>(smallest, std::llround(request.blockSize * 1e7 / rate))
                                : standard;
            auto result = audioClient->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, duration,
                                             duration, &asking.Format, nullptr);
            if (result == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED)
            {
                // A size the device calls unaligned: the size it gave, on a new client.
                UINT32 aligned = 0;
                audioClient->GetBufferSize(&aligned);
                duration = static_cast<REFERENCE_TIME>(std::llround(1e7 * aligned / rate));
                if (!activate(device, error))
                    return false;
                result = audioClient->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, duration,
                                            duration, &asking.Format, nullptr);
            }
            if (FAILED(result))
            {
                error = {result == AUDCLNT_E_DEVICE_IN_USE ? "Another program is using the device on its own."
                         : result == AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED
                             ? "The device doesn't allow exclusive mode; it is turned off in its Sound settings."
                             : "Windows Audio didn't open the device in exclusive mode.",
                         "Initialize", result};
                return false;
            }
            format = asking;
            sample = candidate.sample;
            return true;
        }
        error = {"The device takes no format in exclusive mode at that rate.", "IsFormatSupported"};
        return false;
    }

    void run()
    {
        DWORD index = 0;
        const auto registered = AvSetMmThreadCharacteristicsW(L"Pro Audio", &index);
        scheduled = registered != nullptr ? 1 : 0;

        const HANDLE waits[] = {quit, wake};
        bool lostDevice = false;
        while (true)
        {
            const auto woke = WaitForMultipleObjects(2, waits, FALSE, 2000);
            if (woke == WAIT_OBJECT_0)
                break;
            if (woke != WAIT_OBJECT_0 + 1 || lostDevice)
                continue;
            const auto result = capture ? takeInput() : giveOutput();
            if (FAILED(result))
            {
                lostDevice = true;
                if (auto* to = client.load())
                    to->lost(result == AUDCLNT_E_DEVICE_INVALIDATED ? AudioLoss::unplugged : AudioLoss::other, result);
            }
        }

        if (registered != nullptr)
            AvRevertMmThreadCharacteristics(registered);
    }

    // Every packet waiting, each with its flags and position, until the next is empty.
    HRESULT takeInput()
    {
        const auto frameBytes = format.Format.nBlockAlign;
        const auto bytes = static_cast<std::size_t>(format.Format.wBitsPerSample / 8);
        for (;;)
        {
            UINT32 next = 0;
            auto result = captureClient->GetNextPacketSize(&next);
            if (FAILED(result))
                return result;
            if (next == 0)
                return S_OK;

            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            UINT64 position = 0, time = 0;
            result = captureClient->GetBuffer(&data, &frames, &flags, &position, &time);
            if (result == AUDCLNT_S_BUFFER_EMPTY)
                return S_OK;
            if (FAILED(result))
                return result;

            const auto taken = std::min<std::size_t>(frames, capacity);
            const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == nullptr;
            for (std::size_t c = 0; c < buffers.size(); ++c)
            {
                const auto channel = static_cast<std::size_t>(request.inputChannels[c]);
                auto* into = buffers[c].data();
                for (std::size_t i = 0; i < taken; ++i)
                    into[i] = silent ? 0.0f : toFloat(data + i * frameBytes + channel * bytes, sample);
            }

            AudioSystemCycle cycle;
            cycle.inputs = constPointers.data();
            cycle.inputChannels = static_cast<int>(buffers.size());
            cycle.frames = taken;
            cycle.inputPosition = static_cast<std::int64_t>(position);
            cycle.inputTime = (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) != 0 ? -1 : static_cast<std::int64_t>(time) * 100;
            cycle.inputGap = (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0;
            cycle.inputSilent = silent;
            if (auto* to = client.load())
                to->cycle(cycle);

            result = captureClient->ReleaseBuffer(frames);
            if (FAILED(result))
                return result;
        }
    }

    HRESULT giveOutput()
    {
        UINT32 padding = 0;
        if (!request.exclusive)
            if (const auto result = audioClient->GetCurrentPadding(&padding); FAILED(result))
                return result;
        const auto room = request.exclusive ? bufferFrames : bufferFrames - padding;
        if (room == 0)
            return S_OK;
        const auto frames = std::min<std::size_t>(room, capacity);

        BYTE* data = nullptr;
        auto result = renderClient->GetBuffer(static_cast<UINT32>(frames), &data);
        if (FAILED(result))
            return result;

        for (auto& buffer : buffers)
            std::fill(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(frames), 0.0f);
        AudioSystemCycle cycle;
        cycle.outputs = pointers.data();
        cycle.outputChannels = static_cast<int>(buffers.size());
        cycle.frames = frames;
        cycle.outputTime = -1;
        if (auto* to = client.load())
            to->cycle(cycle);

        // Every channel of the device's frame cleared, then those the stream took filled in.
        const auto frameBytes = format.Format.nBlockAlign;
        const auto bytes = static_cast<std::size_t>(format.Format.wBitsPerSample / 8);
        std::memset(data, 0, frames * frameBytes);
        for (std::size_t c = 0; c < buffers.size(); ++c)
        {
            const auto channel = static_cast<std::size_t>(request.outputChannels[c]);
            const auto* from = buffers[c].data();
            for (std::size_t i = 0; i < frames; ++i)
                fromFloat(from[i], data + i * frameBytes + channel * bytes, sample);
        }
        return renderClient->ReleaseBuffer(static_cast<UINT32>(frames), 0);
    }

    bool capture = false;
    AudioStreamRequest request;
    Com<IAudioClient> audioClient;
    Com<IAudioCaptureClient> captureClient;
    Com<IAudioRenderClient> renderClient;
    Com<IAudioSessionControl> session;
    SessionEvents* events = nullptr;
    WAVEFORMATEXTENSIBLE format{};
    Sample sample = Sample::float32;
    int channels = 0;
    bool raw = false;
    UINT32 bufferFrames = 0, periodFrames = 0;
    HANDLE wake = nullptr, quit = nullptr;
    std::thread thread;
    bool running = false;
    std::atomic<AudioSystemClient*> client{nullptr};
    std::atomic<int> scheduled{-1};
    std::size_t capacity = 0;
    std::vector<std::vector<float>> buffers;
    std::vector<float*> pointers;
    std::vector<const float*> constPointers;
};

// The system's own notices, and the computer's sleep, passed to each listener; removing one waits for calls under way.
class Notices final : public IMMNotificationClient
{
public:
    std::mutex lock;
    std::condition_variable idle;
    std::vector<AudioSystemListener*> listeners;
    int callsUnderWay = 0;

    void tell(AudioSystemChange change)
    {
        std::vector<AudioSystemListener*> now;
        {
            const std::lock_guard hold(lock);
            now = listeners;
            ++callsUnderWay;
        }
        for (auto* listener : now)
            listener->systemChanged(change);
        {
            const std::lock_guard hold(lock);
            --callsUnderWay;
        }
        idle.notify_all();
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(++references); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const auto left = --references;
        if (left == 0)
            delete this;
        return static_cast<ULONG>(left);
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override
    {
        if (IsEqualIID(id, __uuidof(IUnknown)) || IsEqualIID(id, __uuidof(IMMNotificationClient)))
        {
            AddRef();
            *out = static_cast<IMMNotificationClient*>(this);
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }

    // Each only passes on that something changed, never blocking or calling back into the system.
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return told(AudioSystemChange::devices); }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return told(AudioSystemChange::devices); }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return told(AudioSystemChange::devices); }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow, ERole role, LPCWSTR) override
    {
        return role == eConsole ? told(AudioSystemChange::defaults) : S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override
    {
        return told(AudioSystemChange::devices);
    }

private:
    HRESULT told(AudioSystemChange change)
    {
        tell(change);
        return S_OK;
    }

    std::atomic<long> references{1};
};

ULONG CALLBACK powerChanged(PVOID context, ULONG type, PVOID)
{
    auto* notices = static_cast<Notices*>(context);
    if (type == PBT_APMSUSPEND)
        notices->tell(AudioSystemChange::sleeping);
    else if (type == PBT_APMRESUMEAUTOMATIC)
        notices->tell(AudioSystemChange::woke);
    return ERROR_SUCCESS;
}

class WindowsAudioSystem final : public AudioSystem
{
public:
    WindowsAudioSystem() : devices(enumerator()) {}

    ~WindowsAudioSystem() override
    {
        if (listening)
        {
            devices->UnregisterEndpointNotificationCallback(notices);
            if (power != nullptr)
                PowerUnregisterSuspendResumeNotification(power);
        }
        notices->Release();
    }

    std::string name() const override { return "Windows Audio"; }

    std::int64_t now() const noexcept override
    {
        static const auto frequency = [] {
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            return f.QuadPart;
        }();
        LARGE_INTEGER counter;
        QueryPerformanceCounter(&counter);
        return static_cast<std::int64_t>(static_cast<double>(counter.QuadPart) * 1e9 / static_cast<double>(frequency));
    }

    std::vector<std::string> deviceIds() override
    {
        std::vector<std::string> ids;
        Com<IMMDeviceCollection> collection;
        if (!devices || FAILED(devices->EnumAudioEndpoints(eAll, DEVICE_STATE_ACTIVE, collection.put())))
            return ids;
        UINT count = 0;
        collection->GetCount(&count);
        for (UINT i = 0; i < count; ++i)
        {
            Com<IMMDevice> device;
            if (SUCCEEDED(collection->Item(i, device.put())))
                if (auto id = idOf(device.get()); !id.empty())
                    ids.push_back(std::move(id));
        }
        return ids;
    }

    AudioDeviceInfo describe(const std::string& id) override
    {
        AudioDeviceInfo info;
        info.id = id;
        Com<IMMDevice> device;
        if (!devices || FAILED(devices->GetDevice(wide(id).c_str(), device.put())))
        {
            info.answering = false;
            info.problem = "The audio device isn't there.";
            return info;
        }
        const auto flow = flowOf(device.get());
        Com<IPropertyStore> store;
        if (FAILED(device->OpenPropertyStore(STGM_READ, store.put())))
        {
            info.answering = false;
            info.problem = "The audio device didn't answer.";
            return info;
        }
        info.name = textProperty(store.get(), PKEY_Device_FriendlyName);

        WAVEFORMATEXTENSIBLE format{};
        int channels = 2;
        if (deviceFormat(store.get(), format))
        {
            channels = format.Format.nChannels;
            info.currentRate = format.Format.nSamplesPerSec;
            info.sampleRates = {info.currentRate};
            // A shared-mode period is 10 ms unless the device says otherwise once a stream is open.
            info.currentBlock = static_cast<int>(std::lround(info.currentRate / 100.0));
            info.blockSizes = {info.currentBlock};
        }
        auto& names = flow == eCapture ? info.inputNames : info.outputNames;
        for (int c = 0; c < channels; ++c)
            names.push_back((flow == eCapture ? "Input " : "Output ") + std::to_string(c + 1));

        // A Bluetooth headset's hands-free endpoints carry its profile's name.
        info.handsFree = info.name.find("Hands-Free") != std::string::npos;
        if (info.handsFree)
            info.transport = AudioTransport::bluetooth;
        return info;
    }

    std::string defaultDevice(AudioDirection direction) override
    {
        Com<IMMDevice> device;
        if (!devices || FAILED(devices->GetDefaultAudioEndpoint(direction == AudioDirection::input ? eCapture : eRender,
                                                                eConsole, device.put())))
            return {};
        return idOf(device.get());
    }

    std::unique_ptr<AudioSystemStream> open(const std::string& id, const AudioStreamRequest& request,
                                            AudioError& error) override
    {
        Com<IMMDevice> device;
        if (!devices || FAILED(devices->GetDevice(wide(id).c_str(), device.put())))
        {
            error = {"The audio device isn't there.", "open"};
            return nullptr;
        }
        auto stream = std::make_unique<WindowsAudioStream>();
        if (!stream->open(device.get(), flowOf(device.get()) == eCapture, request, error))
            return nullptr;
        return stream;
    }

    void addListener(AudioSystemListener& listener) override
    {
        bool first = false;
        {
            const std::lock_guard hold(notices->lock);
            first = notices->listeners.empty() && !listening;
            notices->listeners.push_back(&listener);
        }
        if (first && devices)
        {
            listening = SUCCEEDED(devices->RegisterEndpointNotificationCallback(notices));
            DEVICE_NOTIFY_SUBSCRIBE_PARAMETERS parameters{&powerChanged, notices};
            PowerRegisterSuspendResumeNotification(DEVICE_NOTIFY_CALLBACK, &parameters, &power);
        }
    }

    void removeListener(AudioSystemListener& listener) override
    {
        std::unique_lock hold(notices->lock);
        std::erase(notices->listeners, &listener);
        notices->idle.wait(hold, [this] { return notices->callsUnderWay == 0; });
    }

private:
    static EDataFlow flowOf(IMMDevice* device)
    {
        Com<IMMEndpoint> endpoint;
        EDataFlow flow = eRender;
        if (SUCCEEDED(device->QueryInterface(__uuidof(IMMEndpoint), reinterpret_cast<void**>(endpoint.put()))))
            endpoint->GetDataFlow(&flow);
        return flow;
    }

    Com<IMMDeviceEnumerator> devices;
    Notices* notices = new Notices;
    bool listening = false;
    HPOWERNOTIFY power = nullptr;
};
}

namespace devices::detail
{
std::vector<std::shared_ptr<AudioSystem>> systemsOfThisComputer()
{
    std::vector<std::shared_ptr<AudioSystem>> systems{std::make_shared<WindowsAudioSystem>()};
#if defined(SYMBOLENGINE_ASIO)
    systems.push_back(asio::detail::makeSystem());
#endif
    return systems;
}
}

}
