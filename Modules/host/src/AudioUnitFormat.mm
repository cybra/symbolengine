#include "symbolengine/host/AudioUnitFormat.h"

#include "PluginFiles.h"

#include <symbolengine/core/Files.h>
#include <symbolengine/core/Paths.h>
#include <symbolengine/core/Text.h>

#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstring>

namespace symbolengine
{

namespace
{

// A bundle's Info.plist, read from its file, as CFBundle keeps what it read of a bundle for as long as the program
// runs; owned by the caller, or null.
CFDictionaryRef infoOf(const std::filesystem::path& bundle)
{
    std::error_code error;
    const auto bytes = files::read(bundle / "Contents" / "Info.plist", error);
    if (error || bytes.empty())
        return nullptr;
    const auto data =
        CFDataCreate(nullptr, reinterpret_cast<const UInt8*>(bytes.data()), static_cast<CFIndex>(bytes.size()));
    const auto read = CFPropertyListCreateWithData(nullptr, data, kCFPropertyListImmutable, nullptr, nullptr);
    CFRelease(data);
    if (read != nullptr && CFGetTypeID(read) != CFDictionaryGetTypeID())
    {
        CFRelease(read);
        return nullptr;
    }
    return static_cast<CFDictionaryRef>(read);
}

std::string textOf(CFTypeRef value)
{
    if (value == nullptr || CFGetTypeID(value) != CFStringGetTypeID())
        return {};
    const auto string = static_cast<CFStringRef>(value);
    const auto size = CFStringGetMaximumSizeForEncoding(CFStringGetLength(string), kCFStringEncodingUTF8) + 1;
    std::string text(static_cast<std::size_t>(size), '\0');
    if (!CFStringGetCString(string, text.data(), size, kCFStringEncodingUTF8))
        return {};
    text.resize(std::strlen(text.c_str()));
    return text;
}

// A four-character code as Info.plist writes it, in the system's old encoding, so codes past ASCII keep their bytes.
std::optional<OSType> codeOf(CFTypeRef value)
{
    if (value == nullptr || CFGetTypeID(value) != CFStringGetTypeID())
        return std::nullopt;
    char bytes[8] = {};
    if (!CFStringGetCString(static_cast<CFStringRef>(value), bytes, sizeof bytes, kCFStringEncodingMacRoman) ||
        std::strlen(bytes) != 4)
        return std::nullopt;
    OSType code = 0;
    for (int i = 0; i < 4; ++i)
        code = code << 8 | static_cast<unsigned char>(bytes[i]);
    return code;
}

// The three codes of an identity PluginIdentity::audioUnit made.
std::optional<AudioComponentDescription> descriptionOf(const PluginIdentity& identity)
{
    const auto parts = text::split(identity.id, "-");
    if (identity.format != "AudioUnit" || parts.size() != 3)
        return std::nullopt;
    std::array<OSType, 3> codes{};
    for (std::size_t i = 0; i < 3; ++i)
    {
        if (parts[i].size() != 8)
            return std::nullopt;
        codes[i] = static_cast<OSType>(std::strtoul(std::string(parts[i]).c_str(), nullptr, 16));
    }
    return AudioComponentDescription{codes[0], codes[1], codes[2], 0, 0};
}

PluginKind kindOf(OSType type)
{
    if (type == kAudioUnitType_Effect || type == kAudioUnitType_MusicEffect)
        return PluginKind::effect;
    if (type == kAudioUnitType_MusicDevice)
        return PluginKind::instrument;
    return PluginKind::other;
}

std::string reason(const char* what, OSStatus status)
{
    return std::string(what) + " (" + std::to_string(status) + ")";
}

// Non-interleaved 32-bit float, one buffer a channel, at the rate.
AudioStreamBasicDescription floatFormat(double rate, UInt32 channels)
{
    AudioStreamBasicDescription format{};
    format.mSampleRate = rate;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = static_cast<AudioFormatFlags>(kAudioFormatFlagsNativeFloatPacked) |
                          static_cast<AudioFormatFlags>(kAudioFormatFlagIsNonInterleaved);
    format.mBytesPerPacket = sizeof(float);
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = sizeof(float);
    format.mChannelsPerFrame = channels;
    format.mBitsPerChannel = 32;
    return format;
}

// One Audio Unit, as the host module holds it.
class AudioUnitPlugin final : public PluginFormatInstance
{
public:
    AudioUnitPlugin(AudioUnit made, PluginFormatClient& told) : unit(made), client(told)
    {
        AudioUnitAddPropertyListener(unit, kAudioUnitProperty_Latency, &AudioUnitPlugin::propertyChanged, this);
    }

    ~AudioUnitPlugin() override
    {
        // In order: its listener gone, anything it holds of audio cleared, uninitialized, disposed, on the main thread.
        AudioUnitRemovePropertyListenerWithUserData(unit, kAudioUnitProperty_Latency, &AudioUnitPlugin::propertyChanged,
                                                    this);
        AudioUnitReset(unit, kAudioUnitScope_Global, 0);
        if (initialized)
            AudioUnitUninitialize(unit);
        AudioComponentInstanceDispose(unit);
    }

    PluginLayout arrange(PluginLayout wanted) override
    {
        if (busCount(kAudioUnitScope_Input) > 0)
            setFormat(kAudioUnitScope_Input, 0, static_cast<UInt32>(wanted.inputs));
        setFormat(kAudioUnitScope_Output, 0, static_cast<UInt32>(wanted.outputs));
        layout = currentLayout();
        return layout;
    }

    bool activate(const PluginSetup& setup, PluginError& error) override
    {
        rate = setup.rate;

        // Its largest block before anything else, read back, as a unit that takes less is given pieces it takes.
        UInt32 largest = static_cast<UInt32>(std::max(1, setup.maximumFrames));
        AudioUnitSetProperty(unit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &largest,
                             sizeof largest);
        maximumFrames = 0;
        UInt32 size = sizeof maximumFrames;
        if (AudioUnitGetProperty(unit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0,
                                 &maximumFrames, &size) != noErr ||
            maximumFrames == 0)
        {
            error = {"It says no largest block.", "activate", 0};
            return false;
        }

        // Every bus's whole format, at the rate, whether or not it already says so, and read back.
        for (const auto scope : {kAudioUnitScope_Input, kAudioUnitScope_Output})
            for (UInt32 bus = 0; bus < busCount(scope); ++bus)
            {
                setFormat(scope, bus, channels(scope, bus));
                if (std::abs(formatOf(scope, bus).mSampleRate - rate) > 0.5)
                {
                    error = {"It doesn't take " + std::to_string(static_cast<long long>(rate)) + " Hz.", "activate", 0};
                    return false;
                }
            }

        // Its input from the host on every input bus, the main one's from the block and the rest silence; and the
        // host's callbacks, set again before every initialize, as a unit may forget them as it is uninitialized.
        for (UInt32 bus = 0; bus < busCount(kAudioUnitScope_Input); ++bus)
        {
            AURenderCallbackStruct callback{&AudioUnitPlugin::input, this};
            AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, bus, &callback,
                                 sizeof callback);
        }
        HostCallbackInfo callbacks{this, &AudioUnitPlugin::beatAndTempo, &AudioUnitPlugin::musicalTime,
                                   &AudioUnitPlugin::transport, &AudioUnitPlugin::transport2};
        AudioUnitSetProperty(unit, kAudioUnitProperty_HostCallbacks, kAudioUnitScope_Global, 0, &callbacks,
                             sizeof callbacks);
        UInt32 offline = setup.offline ? 1 : 0;
        AudioUnitSetProperty(unit, kAudioUnitProperty_OfflineRender, kAudioUnitScope_Global, 0, &offline,
                             sizeof offline);

        if (const auto status = AudioUnitInitialize(unit); status != noErr)
        {
            error = {reason("It couldn't be initialized", status), "activate", status};
            return false;
        }
        initialized = true;

        // Its buses as it was initialized with them, which it may have changed.
        if (currentLayout() != layout)
        {
            AudioUnitUninitialize(unit);
            initialized = false;
            error = {"It changed its buses as it was initialized.", "activate", 0};
            return false;
        }

        outputs.assign(static_cast<std::size_t>(layout.outputs), std::vector<float>(maximumFrames));
        silence.assign(maximumFrames, 0.0f);
        list.assign(offsetof(AudioBufferList, mBuffers) +
                        sizeof(AudioBuffer) * std::max<std::size_t>(1, outputs.size()),
                    std::byte{});
        heldIn.assign(static_cast<std::size_t>(std::max(0, layout.inputs)), std::vector<float>(maximumFrames));
        heldOut.assign(static_cast<std::size_t>(layout.outputs), std::vector<float>(maximumFrames));
        held = 0;
        sampleTime = 0;
        failures = 0;
        return true;
    }

    void deactivate() override
    {
        if (initialized)
            AudioUnitUninitialize(unit);
        initialized = false;
    }

    // A unit fed full blocks through a buffer is that buffer's length later too.
    int latency() override
    {
        return framesOf(property(kAudioUnitProperty_Latency)) + (fixedBlocks ? static_cast<int>(maximumFrames) : 0);
    }

    int tail() override { return framesOf(property(kAudioUnitProperty_TailTime)); }

    void reset() override { AudioUnitReset(unit, kAudioUnitScope_Global, 0); }

    void startProcessing() override {}
    void stopProcessing() override {}

    const char* stopped() const override { return gone; }

    bool process(PluginBlock& block) override
    {
        if (gone != nullptr)
            return false;
        if (fixedBlocks)
            return processHeld(block);

        for (int done = 0; done < block.frames;)
        {
            const auto frames = std::min<UInt32>(maximumFrames, static_cast<UInt32>(block.frames - done));
            const auto status =
                render({block.inputs, block.inputChannels, done, block.frames}, frames, block.time, done);

            // A unit that fails a short block needs every block the same size: from now on it is fed full blocks
            // through a buffer, whose length its latency counts, and is prepared again for it. This block passes.
            if (status == kAudio_ParamError && frames < maximumFrames)
            {
                fixedBlocks = true;
                client.notice(PluginNotice::latency);
                for (int c = 0; c < block.outputChannels; ++c)
                    passDry(block.inputs, block.inputChannels, c, done, block.outputs[c] + done,
                            static_cast<UInt32>(block.frames - done));
                return true;
            }
            if (!take(status, frames, block.inputs, block.inputChannels, done, block.outputs, block.outputChannels,
                      done))
                return false;
            done += static_cast<int>(frames);
        }
        return true;
    }

    void flush(const PluginParameterChange*, int) override {}
    std::vector<PluginParameter> parameters() override { return {}; }
    double parameterValue(std::uint32_t) override { return 0.0; }
    void showParameterValue(std::uint32_t, double) override {}
    std::string parameterText(std::uint32_t, double) override { return {}; }

    std::optional<std::string> state(PluginStateUse, PluginError& error) override
    {
        error = {"An Audio Unit's state isn't kept yet.", "state", 0};
        return std::nullopt;
    }

    bool setState(std::string_view, PluginStateUse, PluginError& error) override
    {
        error = {"An Audio Unit's state isn't kept yet.", "setState", 0};
        return false;
    }

private:
    // Where a render's input comes from: channels of samples, from a frame of them on, so many in all.
    struct Source
    {
        const float* const* channels = nullptr;
        int count = 0;
        int from = 0;
        int frames = 0;
    };

    AudioUnit unit;
    PluginFormatClient& client;
    bool initialized = false;
    const char* gone = nullptr;
    int failures = 0;
    bool fixedBlocks = false;
    double rate = 48000.0;
    UInt32 maximumFrames = 0;
    PluginLayout layout{};
    Float64 sampleTime = 0;
    std::vector<std::vector<float>> outputs;
    std::vector<float> silence;
    std::vector<std::byte> list;

    // For a unit fed full blocks: the input gathered for its next one, the output of its last, and how far along both
    // are.
    std::vector<std::vector<float>> heldIn;
    std::vector<std::vector<float>> heldOut;
    UInt32 held = 0;

    // During a render, on the audio thread: its input, how much of it the unit has pulled, and where the program is in
    // time, which the host's callbacks answer from and refuse outside a render.
    Source source;
    UInt32 pulled = 0;
    const PluginTime* renderTime = nullptr;
    int renderOffset = 0;
    bool toldPlaying = false;

    PluginLayout currentLayout() const
    {
        return {busCount(kAudioUnitScope_Input) > 0 ? static_cast<int>(channels(kAudioUnitScope_Input, 0)) : 0,
                static_cast<int>(channels(kAudioUnitScope_Output, 0))};
    }

    // Renders frames into its own buffers, at the time given, its sample time running on by exactly the frames.
    OSStatus render(Source from, UInt32 frames, const PluginTime* time, int offset)
    {
        source = from;
        pulled = 0;
        renderTime = time;
        renderOffset = offset;

        AudioTimeStamp stamp{};
        stamp.mSampleTime = sampleTime;
        stamp.mFlags = kAudioTimeStampSampleTimeValid;
        if (time != nullptr && time->systemNanoseconds > 0)
        {
            // Host time in the system's ticks, which stop while the computer sleeps, where the steady clock the program
            // gives its time on doesn't: counted back from the ticks now by how long ago the time was.
            static const auto base = [] {
                mach_timebase_info_data_t info{};
                mach_timebase_info(&info);
                return info;
            }();
            const auto ticksNow = mach_absolute_time();
            const auto steadyNow = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       std::chrono::steady_clock::now().time_since_epoch())
                                       .count();
            const auto ago = static_cast<long double>(steadyNow - time->systemNanoseconds) - offset * 1e9L / rate;
            stamp.mHostTime = static_cast<UInt64>(static_cast<long double>(ticksNow) - ago * base.denom / base.numer);
            stamp.mFlags |= kAudioTimeStampHostTimeValid;
        }

        auto* buffers = reinterpret_cast<AudioBufferList*>(list.data());
        buffers->mNumberBuffers = static_cast<UInt32>(outputs.size());
        for (std::size_t c = 0; c < outputs.size(); ++c)
            buffers->mBuffers[c] = {1, frames * static_cast<UInt32>(sizeof(float)), outputs[c].data()};

        AudioUnitRenderActionFlags flags = 0;
        const auto status = AudioUnitRender(unit, &flags, &stamp, 0, frames, buffers);
        source = {};
        renderTime = nullptr;

        // Its frames are used up whether or not it rendered them; a unit given the same time again may give back what
        // it gave the last time, as Apple's SDK does.
        sampleTime += frames;
        return status;
    }

    // Takes what a render gave into the destination: wherever the unit put each channel, silence for one it left
    // unwritten, whatever it said of silence; a failed render never heard, the input passing as it came. False once
    // the unit can't be called again.
    bool take(OSStatus status, UInt32 frames, const float* const* inputs, int inputCount, int inputFrom,
              float* const* destination, int destinationCount, int destinationFrom)
    {
        if (status == kAudioComponentErr_InstanceInvalidated || status == kAudioComponentErr_InstanceTimedOut)
        {
            gone = "The system says its instance is gone.";
            return false;
        }
        const auto* buffers = reinterpret_cast<const AudioBufferList*>(list.data());
        for (int c = 0; c < destinationCount; ++c)
        {
            auto* to = destination[c] + destinationFrom;
            if (status != noErr)
            {
                passDry(inputs, inputCount, c, inputFrom, to, frames);
                continue;
            }
            const auto& written = buffers->mBuffers[static_cast<std::size_t>(c)];
            const auto count = static_cast<std::size_t>(c) < buffers->mNumberBuffers && written.mData != nullptr
                                   ? std::min<UInt32>(frames, written.mDataByteSize / sizeof(float))
                                   : 0;
            std::copy_n(static_cast<const float*>(written.mData), count, to);
            std::fill(to + count, to + frames, 0.0f);
        }
        if (status == noErr)
            failures = 0;
        else if (++failures >= 3)
        {
            gone = "It failed three blocks in a row.";
            return false;
        }
        return true;
    }

    static void passDry(const float* const* inputs, int inputCount, int channel, int from, float* to, UInt32 frames)
    {
        if (channel < inputCount)
            std::copy_n(inputs[channel] + from, frames, to);
        else
            std::fill_n(to, frames, 0.0f);
    }

    // A unit fed full blocks: the block's input gathered into the next full one and its output given from the last,
    // rendering each time one is full.
    bool processHeld(PluginBlock& block)
    {
        std::vector<const float*> in(heldIn.size());
        std::vector<float*> out(heldOut.size());
        for (std::size_t c = 0; c < heldIn.size(); ++c)
            in[c] = heldIn[c].data();
        for (std::size_t c = 0; c < heldOut.size(); ++c)
            out[c] = heldOut[c].data();

        for (int done = 0; done < block.frames;)
        {
            const auto frames = std::min<UInt32>(maximumFrames - held, static_cast<UInt32>(block.frames - done));
            for (std::size_t c = 0; c < heldIn.size(); ++c)
                passDry(block.inputs, block.inputChannels, static_cast<int>(c), done, heldIn[c].data() + held, frames);
            for (int c = 0; c < block.outputChannels; ++c)
                if (static_cast<std::size_t>(c) < heldOut.size())
                    std::copy_n(heldOut[static_cast<std::size_t>(c)].data() + held, frames, block.outputs[c] + done);
                else
                    std::fill_n(block.outputs[c] + done, frames, 0.0f);
            held += frames;
            done += static_cast<int>(frames);

            if (held == maximumFrames)
            {
                const auto status = render({in.data(), static_cast<int>(in.size()), 0, static_cast<int>(maximumFrames)},
                                           maximumFrames, block.time, done);
                held = 0;
                if (!take(status, maximumFrames, in.data(), static_cast<int>(in.size()), 0, out.data(),
                          static_cast<int>(out.size()), 0))
                    return false;
            }
        }
        return true;
    }

    UInt32 busCount(AudioUnitScope scope) const
    {
        UInt32 count = 0;
        UInt32 size = sizeof count;
        return AudioUnitGetProperty(unit, kAudioUnitProperty_ElementCount, scope, 0, &count, &size) == noErr ? count
                                                                                                             : 0;
    }

    AudioStreamBasicDescription formatOf(AudioUnitScope scope, UInt32 bus) const
    {
        AudioStreamBasicDescription format{};
        UInt32 size = sizeof format;
        AudioUnitGetProperty(unit, kAudioUnitProperty_StreamFormat, scope, bus, &format, &size);
        return format;
    }

    UInt32 channels(AudioUnitScope scope, UInt32 bus) const { return formatOf(scope, bus).mChannelsPerFrame; }

    void setFormat(AudioUnitScope scope, UInt32 bus, UInt32 count)
    {
        const auto format = floatFormat(rate, count);
        AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, scope, bus, &format, sizeof format);
    }

    Float64 property(AudioUnitPropertyID id) const
    {
        Float64 seconds = 0;
        UInt32 size = sizeof seconds;
        return AudioUnitGetProperty(unit, id, kAudioUnitScope_Global, 0, &seconds, &size) == noErr ? seconds : 0.0;
    }

    int framesOf(Float64 seconds) const
    {
        return static_cast<int>(std::clamp(std::lround(seconds * rate), 0L, static_cast<long>(INT32_MAX)));
    }

    // The unit pulling its input, on the audio thread: exactly the frames it asks for, from where the last pull of
    // this render left off, silence past the block and on every bus but the main one.
    static OSStatus input(void* context, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32 bus, UInt32 frames,
                          AudioBufferList* into)
    {
        auto& self = *static_cast<AudioUnitPlugin*>(context);
        const auto& from = self.source;
        const auto at = static_cast<UInt32>(from.from) + self.pulled;
        const auto available = from.channels != nullptr && bus == 0 && at < static_cast<UInt32>(from.frames)
                                   ? std::min(frames, static_cast<UInt32>(from.frames) - at)
                                   : 0;
        for (UInt32 c = 0; c < into->mNumberBuffers; ++c)
        {
            auto& buffer = into->mBuffers[c];
            if (buffer.mData == nullptr)
            {
                // A unit that leaves the buffer to the host is given silence it may read but never write to.
                buffer.mData = self.silence.data();
                buffer.mDataByteSize =
                    std::min<UInt32>(frames, static_cast<UInt32>(self.silence.size())) * sizeof(float);
                continue;
            }
            auto* to = static_cast<float*>(buffer.mData);
            const auto room = std::min<UInt32>(frames, buffer.mDataByteSize / sizeof(float));
            const auto copied = static_cast<int>(c) < from.count ? std::min(available, room) : 0;
            if (copied > 0)
                std::copy_n(from.channels[c] + at, copied, to);
            std::fill(to + copied, to + room, 0.0f);
        }
        if (bus == 0)
            self.pulled += frames;
        return noErr;
    }

    static void propertyChanged(void* context, AudioUnit, AudioUnitPropertyID id, AudioUnitScope, AudioUnitElement)
    {
        if (id == kAudioUnitProperty_Latency)
            static_cast<AudioUnitPlugin*>(context)->client.notice(PluginNotice::latency);
    }

    // The host's callbacks, answered from where the program is in time for the render under way, and refused outside
    // one: 120 beats a minute in 4/4 where the program has no tempo, and never a cycle.
    struct Musical
    {
        double tempo = 120;
        double beat = 0;
        int numerator = 4;
        int denominator = 4;
        double frame = 0;
    };

    std::optional<Musical> musical() const
    {
        if (renderTime == nullptr)
            return std::nullopt;
        Musical now;
        now.tempo = renderTime->tempo > 0 ? renderTime->tempo : 120.0;
        now.numerator = renderTime->numerator > 0 ? renderTime->numerator : 4;
        now.denominator = renderTime->denominator > 0 ? renderTime->denominator : 4;
        now.frame = static_cast<double>(std::max<std::int64_t>(0, renderTime->position + renderOffset));
        now.beat = now.frame / rate * now.tempo / 60.0;
        return now;
    }

    static OSStatus beatAndTempo(void* context, Float64* beat, Float64* tempo)
    {
        const auto now = static_cast<const AudioUnitPlugin*>(context)->musical();
        if (!now)
            return kAudioUnitErr_CannotDoInCurrentContext;
        if (beat != nullptr)
            *beat = now->beat;
        if (tempo != nullptr)
            *tempo = now->tempo;
        return noErr;
    }

    static OSStatus musicalTime(void* context, UInt32* toNextBeat, Float32* numerator, UInt32* denominator,
                                Float64* downbeat)
    {
        const auto& self = *static_cast<const AudioUnitPlugin*>(context);
        const auto now = self.musical();
        if (!now)
            return kAudioUnitErr_CannotDoInCurrentContext;
        if (toNextBeat != nullptr)
            *toNextBeat =
                static_cast<UInt32>(std::lround((std::ceil(now->beat) - now->beat) * 60.0 / now->tempo * self.rate));
        if (numerator != nullptr)
            *numerator = static_cast<Float32>(now->numerator);
        if (denominator != nullptr)
            *denominator = static_cast<UInt32>(now->denominator);
        if (downbeat != nullptr)
        {
            const auto perBar = now->numerator * 4.0 / now->denominator;
            *downbeat = std::floor(now->beat / perBar) * perBar;
        }
        return noErr;
    }

    static OSStatus transport2(void* context, Boolean* playing, Boolean* recording, Boolean* changed, Float64* sample,
                               Boolean* cycling, Float64* cycleStart, Float64* cycleEnd)
    {
        auto& self = *static_cast<AudioUnitPlugin*>(context);
        const auto now = self.musical();
        if (!now)
            return kAudioUnitErr_CannotDoInCurrentContext;
        const bool isPlaying = self.renderTime->playing;
        if (playing != nullptr)
            *playing = isPlaying;
        if (recording != nullptr)
            *recording = self.renderTime->recording;
        if (changed != nullptr)
            *changed = isPlaying != self.toldPlaying;
        self.toldPlaying = isPlaying;
        if (sample != nullptr)
            *sample = now->frame;
        if (cycling != nullptr)
            *cycling = false;
        if (cycleStart != nullptr)
            *cycleStart = 0;
        if (cycleEnd != nullptr)
            *cycleEnd = 0;
        return noErr;
    }

    static OSStatus transport(void* context, Boolean* playing, Boolean* changed, Float64* sample, Boolean* cycling,
                              Float64* cycleStart, Float64* cycleEnd)
    {
        return transport2(context, playing, nullptr, changed, sample, cycling, cycleStart, cycleEnd);
    }
};

}

struct AudioUnitFormat::Shared
{
    Options options;
};

AudioUnitFormat::AudioUnitFormat(Options options) : shared(std::make_shared<Shared>())
{
    shared->options = std::move(options);
}

AudioUnitFormat::~AudioUnitFormat() = default;

std::string AudioUnitFormat::name() const
{
    return "AudioUnit";
}

std::vector<PluginFolder> AudioUnitFormat::folders() const
{
    std::vector<PluginFolder> folders{{"/Library/Audio/Plug-Ins/Components", false}};
    std::error_code error;
    if (const auto home = paths::systemFolder(paths::Folder::home, error); !error)
        folders.push_back({home / "Library" / "Audio" / "Plug-Ins" / "Components", true});
    return folders;
}

bool AudioUnitFormat::isPluginFile(const std::filesystem::path& path) const
{
    std::error_code error;
    return text::equalsIgnoringCaseAscii(paths::toUtf8(path.extension()), ".component") &&
           std::filesystem::is_directory(path, error);
}

std::optional<PluginFileFacts> AudioUnitFormat::examine(const std::filesystem::path& file, PluginError& error) const
{
    error = {};
    const auto info = infoOf(file);
    const auto executable = info != nullptr ? textOf(CFDictionaryGetValue(info, CFSTR("CFBundleExecutable"))) : "";
    if (info != nullptr)
        CFRelease(info);
    const auto binary = file / "Contents" / "MacOS" / paths::fromUtf8(executable);
    std::error_code readError;
    if (executable.empty() || !std::filesystem::is_regular_file(binary, readError))
    {
        error = {"It holds no binary its Info.plist names.", "read", 0};
        return std::nullopt;
    }

    PluginFileFacts facts;
    facts.stamp.size = static_cast<std::int64_t>(std::filesystem::file_size(binary, readError));
    facts.stamp.modified = pluginFiles::modifiedNanoseconds(binary, readError);
    if (readError)
    {
        error = {"Its binary can't be read: " + readError.message(), "read", 0};
        return std::nullopt;
    }
    facts.stamp.version = pluginFiles::declaredVersion(file, binary);
    facts.architectures = pluginFiles::architecturesOf(binary);
    return facts;
}

std::vector<PluginDescription> AudioUnitFormat::pluginsIn(const std::filesystem::path& file, PluginError& error)
{
    error = {};
    const auto info = infoOf(file);
    const auto list =
        info != nullptr ? static_cast<CFArrayRef>(CFDictionaryGetValue(info, CFSTR("AudioComponents"))) : nullptr;
    if (list == nullptr || CFGetTypeID(list) != CFArrayGetTypeID())
    {
        if (info != nullptr)
            CFRelease(info);
        error = {"Its Info.plist declares no Audio Units.", "read", 0};
        return {};
    }

    std::vector<PluginDescription> plugins;
    for (CFIndex i = 0; i < CFArrayGetCount(list); ++i)
    {
        const auto entry = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(list, i));
        if (CFGetTypeID(entry) != CFDictionaryGetTypeID())
            continue;
        const auto type = codeOf(CFDictionaryGetValue(entry, CFSTR("type")));
        const auto subtype = codeOf(CFDictionaryGetValue(entry, CFSTR("subtype")));
        const auto manufacturer = codeOf(CFDictionaryGetValue(entry, CFSTR("manufacturer")));
        if (!type || !subtype || !manufacturer)
            continue;

        PluginDescription plugin;
        plugin.identity = PluginIdentity::audioUnit(*type, *subtype, *manufacturer);
        plugin.location = file;
        plugin.kind = kindOf(*type);

        // "Maker: Name", split at its first colon; a name with none is all name.
        const auto named = textOf(CFDictionaryGetValue(entry, CFSTR("name")));
        if (const auto colon = named.find(':'); colon != std::string::npos)
        {
            plugin.maker = std::string(text::trim(std::string_view(named).substr(0, colon)));
            plugin.name = std::string(text::trim(std::string_view(named).substr(colon + 1)));
        }
        else
            plugin.name = std::string(text::trim(named));

        SInt64 version = 0;
        if (const auto value = static_cast<CFNumberRef>(CFDictionaryGetValue(entry, CFSTR("version")));
            value != nullptr && CFGetTypeID(value) == CFNumberGetTypeID() &&
            CFNumberGetValue(value, kCFNumberSInt64Type, &version))
            plugin.version = std::to_string((version >> 16) & 0xFFFF) + "." + std::to_string((version >> 8) & 0xFF) +
                             "." + std::to_string(version & 0xFF);
        plugins.push_back(std::move(plugin));
    }
    CFRelease(info);
    return plugins;
}

void AudioUnitFormat::make(const PluginDescription& plugin, PluginFormatClient& client,
                           std::function<void(std::unique_ptr<PluginFormatInstance>, PluginError)> done)
{
    const auto description = descriptionOf(plugin.identity);
    const auto component = description ? AudioComponentFindNext(nullptr, &*description) : nullptr;
    if (component == nullptr)
    {
        done(nullptr, {"It isn't among the Audio Units the system knows.", "make", 0});
        return;
    }

    // Made without blocking the main thread, its completion handed to the main thread whatever thread it comes on.
    auto post = shared->options.post;
    auto* const told = &client;
    AudioComponentInstantiate(component, 0, ^(AudioComponentInstance instance, OSStatus status) {
      post([instance, status, done, told] {
          if (status != noErr || instance == nullptr)
              done(nullptr, {reason("It couldn't be made", status), "make", status});
          else
              done(std::make_unique<AudioUnitPlugin>(instance, *told), {});
      });
    });
}

}
