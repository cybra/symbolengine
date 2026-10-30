#include "symbolengine/host/AudioUnitFormat.h"

#include "PluginFiles.h"

#include <symbolengine/core/Files.h>
#include <symbolengine/core/Paths.h>
#include <symbolengine/core/System.h>
#include <symbolengine/core/Text.h>

#import <AppKit/AppKit.h>
#import <AudioToolbox/AUCocoaUIView.h>

#include <AudioToolbox/AudioToolbox.h>
#include <AudioToolbox/AudioUnitUtilities.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <mach/mach_time.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits.h>
#include <limits>
#include <memory>
#include <mutex>

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

// What the host needs of a parameter, on any thread, to turn the program's value from 0 to 1 into the unit's own and
// back: its range, its steps as VST3 counts them, and its unit; and for one the host can't set, the value last
// reported, which only the audio thread uses.
struct ParameterRange
{
    AudioUnitParameterID id = 0;
    AudioUnitParameterValue minimum = 0;
    AudioUnitParameterValue maximum = 1;
    int steps = 0;
    AudioUnitParameterUnit unit = kAudioUnitParameterUnit_Generic;
    bool writable = true;
    mutable AudioUnitParameterValue reported = std::numeric_limits<AudioUnitParameterValue>::quiet_NaN();
};

double normalized(const ParameterRange& range, AudioUnitParameterValue value)
{
    if (range.maximum <= range.minimum)
        return 0.0;
    return std::clamp(
        (static_cast<double>(value) - range.minimum) / (static_cast<double>(range.maximum) - range.minimum), 0.0, 1.0);
}

AudioUnitParameterValue unitValue(const ParameterRange& range, double value)
{
    auto position = std::clamp(value, 0.0, 1.0);
    if (range.steps > 0)
        position = std::round(position * range.steps) / range.steps;
    return static_cast<AudioUnitParameterValue>(range.minimum +
                                                position * (static_cast<double>(range.maximum) - range.minimum));
}

int stepsOf(const AudioUnitParameterInfo& info)
{
    if (info.unit == kAudioUnitParameterUnit_Boolean)
        return 1;
    if (info.unit == kAudioUnitParameterUnit_Indexed)
        return std::max(0, static_cast<int>(std::lround(info.maxValue - info.minValue)));
    return 0;
}

std::string unitsOf(const AudioUnitParameterInfo& info)
{
    switch (info.unit)
    {
        case kAudioUnitParameterUnit_Percent: return "%";
        case kAudioUnitParameterUnit_Seconds: return "s";
        case kAudioUnitParameterUnit_Milliseconds: return "ms";
        case kAudioUnitParameterUnit_Hertz: return "Hz";
        case kAudioUnitParameterUnit_Decibels: return "dB";
        case kAudioUnitParameterUnit_Cents:
        case kAudioUnitParameterUnit_AbsoluteCents: return "cents";
        case kAudioUnitParameterUnit_Octaves: return "octaves";
        case kAudioUnitParameterUnit_BPM: return "BPM";
        case kAudioUnitParameterUnit_Beats: return "beats";
        case kAudioUnitParameterUnit_SampleFrames: return "samples";
        case kAudioUnitParameterUnit_Degrees: return "\u00B0";
        case kAudioUnitParameterUnit_Meters: return "m";
        case kAudioUnitParameterUnit_CustomUnit: return textOf(info.unitName);
        default: return {};
    }
}

class AudioUnitEditor;

// One Audio Unit, as the host module holds it.
class AudioUnitPlugin final : public PluginFormatInstance
{
public:
    AudioUnitPlugin(AudioUnit made, PluginFormatClient& told) : unit(made), client(told)
    {
        for (const auto id : noticed)
            AudioUnitAddPropertyListener(unit, id, &AudioUnitPlugin::propertyChanged, this);

        // Its own edits, as its window makes them, heard on a queue of their own: the program's main thread may not
        // turn the run loop the system would otherwise tell them on.
        listenerQueue = dispatch_queue_create("SymbolEngine Audio Unit edits", DISPATCH_QUEUE_SERIAL);
        AUEventListenerCreateWithDispatchQueue(
            &listener, 0.0, 0.0, listenerQueue,
            ^(void*, const AudioUnitEvent* event, UInt64, AudioUnitParameterValue value) {
              heard(*event, value);
            });
    }

    ~AudioUnitPlugin() override
    {
        // In order: its listeners gone, and any edit being told finished; anything it holds of audio cleared,
        // uninitialized, disposed, on the main thread.
        if (listener != nullptr)
            AUListenerDispose(listener);
        dispatch_sync(listenerQueue, ^{
                      });
        dispatch_release(listenerQueue);
        for (const auto id : noticed)
            AudioUnitRemovePropertyListenerWithUserData(unit, id, &AudioUnitPlugin::propertyChanged, this);
        AudioUnitReset(unit, kAudioUnitScope_Global, 0);
        if (initialized)
            AudioUnitUninitialize(unit);
        AudioComponentInstanceDispose(unit);
        if (heldState != nullptr)
            CFRelease(heldState);
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
        offline = setup.offline;

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
        UInt32 rendersOffline = offline ? 1 : 0;
        AudioUnitSetProperty(unit, kAudioUnitProperty_OfflineRender, kAudioUnitScope_Global, 0, &rendersOffline,
                             sizeof rendersOffline);

        // A state given while it wasn't initialized, given again now its formats and largest block are set.
        if (heldState != nullptr)
            give(heldState, heldAsDocument);

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

        // And once more if it didn't take, read back to see, as some units take a state only once initialized.
        if (heldState != nullptr)
        {
            OSStatus status = noErr;
            const auto now = stateOf(heldAsDocument, status);
            if (now == nullptr || !CFEqual(now, ownPart(heldState)))
                give(heldState, heldAsDocument);
            if (now != nullptr)
                CFRelease(now);
            CFRelease(heldState);
            heldState = nullptr;
            valuesChanged();
        }

        outputs.assign(static_cast<std::size_t>(layout.outputs), std::vector<float>(maximumFrames));
        silence.assign(maximumFrames, 0.0f);
        list.assign(offsetof(AudioBufferList, mBuffers) +
                        sizeof(AudioBuffer) * std::max<std::size_t>(1, outputs.size()),
                    std::byte{});
        heldIn.assign(static_cast<std::size_t>(std::max(0, layout.inputs)), std::vector<float>(maximumFrames));
        heldOut.assign(static_cast<std::size_t>(layout.outputs), std::vector<float>(maximumFrames));
        heldInChannels.assign(heldIn.size(), nullptr);
        heldOutChannels.assign(heldOut.size(), nullptr);
        for (std::size_t c = 0; c < heldIn.size(); ++c)
            heldInChannels[c] = heldIn[c].data();
        for (std::size_t c = 0; c < heldOut.size(); ++c)
            heldOutChannels[c] = heldOut[c].data();
        events.assign(maximumBlockChanges, {});
        heldEvents.assign(maximumBlockChanges, {});
        heldEventCount = 0;
        held = 0;
        sampleTime = 0;
        failures = 0;

        // An open editor's view, taken out as it was uninitialized, put back.
        viewBack();
        return true;
    }

    // An open editor's view taken out first, as a view keeps its unit and calls it while it lives.
    void deactivate() override
    {
        if (initialized)
        {
            viewOut();
            AudioUnitUninitialize(unit);
        }
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

        // A discontinuity clears what the unit holds of the audio before it: rendering offline, at once, on this
        // thread, between renders; in real time, from the main thread, the unit rendering on meanwhile, so what it held
        // may be heard for as long as that takes. The host's own buffer for a unit fed full blocks is cleared at once.
        if (block.time != nullptr && block.time->discontinuity)
        {
            clearHeld();
            if (offline)
                AudioUnitReset(unit, kAudioUnitScope_Global, 0);
            else
                client.notice(PluginNotice::reset);
        }

        if (fixedBlocks)
            return processHeld(block);

        int next = 0;
        for (int done = 0; done < block.frames;)
        {
            const auto frames = std::min<UInt32>(maximumFrames, static_cast<UInt32>(block.frames - done));

            // The block's changes for this piece, each at its frame in it; the last piece takes any left.
            const bool last = done + static_cast<int>(frames) >= block.frames;
            UInt32 count = 0;
            for (; next < block.changeCount && (last || block.changes[next].offset < done + static_cast<int>(frames));
                 ++next)
                if (const auto* const range = rangeOf(block.changes[next].id); range != nullptr && range->writable)
                    events[count++] = eventOf(*range, block.changes[next].value,
                                              static_cast<UInt32>(std::clamp(block.changes[next].offset - done, 0,
                                                                             static_cast<int>(frames) - 1)));

            const auto status = render({block.inputs, block.inputChannels, done, block.frames}, frames, block.time,
                                       done, events.data(), count);

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
        report(block);
        return true;
    }

    // An Audio Unit has one value for each parameter, which its processing and its window share. While it is
    // initialized, the program's values reach it only through its render, just before it, or handed over while nothing
    // plays it, never by a call from another thread while it may render, as some units lock their render against such
    // calls; until then they are set on it straight away.
    void flush(const PluginParameterChange* changes, int count) override
    {
        for (int i = 0; i < count; ++i)
            set(changes[i].id, changes[i].value);
    }

    void showParameterValue(std::uint32_t id, double value) override
    {
        if (!initialized)
            set(id, value);
    }

    double parameterValue(std::uint32_t id) override
    {
        const auto* const range = rangeOf(id);
        AudioUnitParameterValue value = 0;
        if (range == nullptr || AudioUnitGetParameter(unit, id, kAudioUnitScope_Global, 0, &value) != noErr)
            return 0.0;
        return normalized(*range, value);
    }

    // Its global parameters, which are the ones a host shows; each one's edits listened for from then on.
    std::vector<PluginParameter> parameters() override
    {
        std::vector<PluginParameter> found;
        auto made = std::make_unique<std::vector<ParameterRange>>();
        for (const auto id : parameterIds())
        {
            AudioUnitParameterInfo info{};
            UInt32 size = sizeof info;
            if (AudioUnitGetProperty(unit, kAudioUnitProperty_ParameterInfo, kAudioUnitScope_Global, id, &info,
                                     &size) != noErr)
                continue;

            const ParameterRange range{id,
                                       info.minValue,
                                       info.maxValue,
                                       stepsOf(info),
                                       info.unit,
                                       (info.flags & kAudioUnitParameterFlag_IsWritable) != 0};
            PluginParameter parameter;
            parameter.id = id;
            parameter.name = (info.flags & kAudioUnitParameterFlag_HasCFNameString) != 0 && info.cfNameString != nullptr
                                 ? textOf(info.cfNameString)
                                 : std::string(info.name, strnlen(info.name, sizeof info.name));
            parameter.units = unitsOf(info);
            parameter.defaultValue = normalized(range, info.defaultValue);
            parameter.steps = range.steps;
            parameter.readOnly = !range.writable;
            parameter.automatable = range.writable && (info.flags & kAudioUnitParameterFlag_NonRealTime) == 0;
            if ((info.flags & kAudioUnitParameterFlag_CFNameRelease) != 0)
            {
                if ((info.flags & kAudioUnitParameterFlag_HasCFNameString) != 0 && info.cfNameString != nullptr)
                    CFRelease(info.cfNameString);
                if (info.unit == kAudioUnitParameterUnit_CustomUnit && info.unitName != nullptr)
                    CFRelease(info.unitName);
            }
            found.push_back(std::move(parameter));
            made->push_back(range);
        }

        std::sort(made->begin(), made->end(), [](const auto& a, const auto& b) { return a.id < b.id; });
        ranges = made.get();
        rangeTables.push_back(std::move(made));
        listenTo(found);
        return found;
    }

    // A step's own name where it has one, the unit's text for a value where it gives one, else the value.
    std::string parameterText(std::uint32_t id, double value) override
    {
        const auto* const range = rangeOf(id);
        if (range == nullptr)
            return {};
        const auto own = unitValue(*range, value);

        if (range->unit == kAudioUnitParameterUnit_Indexed)
        {
            CFArrayRef names = nullptr;
            UInt32 size = sizeof names;
            if (AudioUnitGetProperty(unit, kAudioUnitProperty_ParameterValueStrings, kAudioUnitScope_Global, id, &names,
                                     &size) == noErr &&
                names != nullptr)
            {
                const auto index = static_cast<CFIndex>(std::lround(own - range->minimum));
                auto text =
                    index >= 0 && index < CFArrayGetCount(names) ? textOf(CFArrayGetValueAtIndex(names, index)) : "";
                CFRelease(names);
                if (!text.empty())
                    return text;
            }
        }

        AudioUnitParameterValue asked = own;
        AudioUnitParameterStringFromValue text{id, &asked, nullptr};
        UInt32 size = sizeof text;
        if (AudioUnitGetProperty(unit, kAudioUnitProperty_ParameterStringFromValue, kAudioUnitScope_Global, 0, &text,
                                 &size) == noErr &&
            text.outString != nullptr)
        {
            auto given = textOf(text.outString);
            CFRelease(text.outString);
            if (!given.empty())
                return given;
        }

        if (range->unit == kAudioUnitParameterUnit_Boolean)
            return own >= 0.5f ? "On" : "Off";
        char number[32];
        std::snprintf(number, sizeof number, "%.2f", static_cast<double>(own));
        return number;
    }

    // Its state as a binary property list: for a document, its document form where it has one, and otherwise its class
    // info; for a copy made to render through, with every value it can be set to, set again after it, as a unit's
    // state may leave one out.
    std::optional<std::string> state(PluginStateUse use, PluginError& error) override
    {
        OSStatus status = noErr;
        auto info = stateOf(use == PluginStateUse::document, status);
        if (info == nullptr)
        {
            error = {reason("It gave no state", status), "state", status};
            return std::nullopt;
        }
        if (use == PluginStateUse::copy)
            info = withValues(info);
        const auto data = CFPropertyListCreateData(nullptr, info, kCFPropertyListBinaryFormat_v1_0, 0, nullptr);
        CFRelease(info);
        if (data == nullptr)
        {
            error = {"Its state couldn't be written as a property list.", "state", 0};
            return std::nullopt;
        }
        std::string bytes(reinterpret_cast<const char*>(CFDataGetBytePtr(data)),
                          static_cast<std::size_t>(CFDataGetLength(data)));
        CFRelease(data);
        return bytes;
    }

    // A document's state given as one, which a unit may take differently, keeping settings of its own; a unit that
    // doesn't tell them apart takes it as any other. Given while it isn't initialized, the state is kept to give again
    // once it is, as some units take one only then.
    bool setState(std::string_view state, PluginStateUse use, PluginError& error) override
    {
        const auto data =
            CFDataCreate(nullptr, reinterpret_cast<const UInt8*>(state.data()), static_cast<CFIndex>(state.size()));
        const auto given = CFPropertyListCreateWithData(nullptr, data, kCFPropertyListImmutable, nullptr, nullptr);
        CFRelease(data);
        if (given == nullptr)
        {
            error = {"It isn't an Audio Unit's state.", "setState", 0};
            return false;
        }

        const bool asDocument = use == PluginStateUse::document;
        if (const auto status = give(given, asDocument); status != noErr)
        {
            CFRelease(given);
            error = {reason("It didn't take the state", status), "setState", status};
            return false;
        }
        if (heldState != nullptr)
            CFRelease(heldState);
        heldState = nullptr;
        if (initialized)
            CFRelease(given);
        else
        {
            heldState = given;
            heldAsDocument = asDocument;
        }
        valuesChanged();
        return true;
    }

    // Whether it has a Cocoa view, asked of its description without one being made.
    bool hasEditor() override
    {
        UInt32 size = 0;
        Boolean writable = false;
        return AudioUnitGetPropertyInfo(unit, kAudioUnitProperty_CocoaUI, kAudioUnitScope_Global, 0, &size,
                                        &writable) == noErr &&
               size >= sizeof(AudioUnitCocoaViewInfo);
    }

    std::unique_ptr<PluginFormatEditor> openEditor(void* parent, PluginEditorFrame& frame, PluginError& error) override;

    AudioUnit handle() const { return unit; }

    // The editor open on it, told as it is uninitialized and initialized again.
    AudioUnitEditor* editor = nullptr;

private:
    void viewOut();
    void viewBack();

    // Where a render's input comes from: channels of samples, from a frame of them on, so many in all.
    struct Source
    {
        const float* const* channels = nullptr;
        int count = 0;
        int from = 0;
        int frames = 0;
    };

    // The properties it tells of that the host follows.
    static constexpr AudioUnitPropertyID noticed[] = {kAudioUnitProperty_Latency, kAudioUnitProperty_ParameterList};

    AudioUnit unit;
    PluginFormatClient& client;
    dispatch_queue_t listenerQueue = nullptr;
    AUEventListenerRef listener = nullptr;
    std::vector<AudioUnitParameterID> listened;
    bool initialized = false;

    // A state given while it wasn't initialized, to give again once it is, and whether as a document's.
    CFPropertyListRef heldState = nullptr;
    bool heldAsDocument = false;
    const char* gone = nullptr;
    int failures = 0;
    bool fixedBlocks = false;
    bool offline = false;
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
    std::vector<const float*> heldInChannels;
    std::vector<float*> heldOutChannels;
    UInt32 held = 0;

    // Its parameters' ranges, sorted by ID, which the audio thread and the listener's queue read while the main thread
    // may make new ones; every one kept for as long as the unit is.
    std::vector<std::unique_ptr<const std::vector<ParameterRange>>> rangeTables;
    std::atomic<const std::vector<ParameterRange>*> ranges{nullptr};

    // The changes scheduled for a render; and for a unit fed full blocks, those gathered for its next one, at their
    // frames in it.
    std::vector<AudioUnitParameterEvent> events;
    std::vector<AudioUnitParameterEvent> heldEvents;
    UInt32 heldEventCount = 0;

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

    // Renders frames into its own buffers, at the time given, with the changes scheduled for it, its sample time
    // running on by exactly the frames.
    OSStatus render(Source from, UInt32 frames, const PluginTime* time, int offset,
                    const AudioUnitParameterEvent* changes, UInt32 changeCount)
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

        if (changeCount > 0)
            AudioUnitScheduleParameters(unit, changes, changeCount);
        AudioUnitRenderActionFlags flags = 0;
        const auto status = AudioUnitRender(unit, &flags, &stamp, 0, frames, buffers);
        source = {};
        renderTime = nullptr;

        // The values of a render that failed, which a unit drops with it, are set at once, so its processing still
        // has them.
        if (status != noErr)
            for (UInt32 i = 0; i < changeCount; ++i)
                AudioUnitSetParameter(unit, changes[i].parameter, kAudioUnitScope_Global, 0,
                                      changes[i].eventValues.immediate.value, 0);

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

    // A unit fed full blocks: the block's input and changes gathered into the next full one and its output given from
    // the last, rendering each time one is full.
    bool processHeld(PluginBlock& block)
    {
        int next = 0;
        for (int done = 0; done < block.frames;)
        {
            const auto frames = std::min<UInt32>(maximumFrames - held, static_cast<UInt32>(block.frames - done));
            const bool last = done + static_cast<int>(frames) >= block.frames;
            for (; next < block.changeCount && (last || block.changes[next].offset < done + static_cast<int>(frames));
                 ++next)
                if (const auto* const range = rangeOf(block.changes[next].id); range != nullptr && range->writable)
                    holdChange(*range, block.changes[next].value,
                               held + static_cast<UInt32>(std::clamp(block.changes[next].offset - done, 0,
                                                                     static_cast<int>(frames) - 1)));

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
                const auto status = render({heldInChannels.data(), static_cast<int>(heldInChannels.size()), 0,
                                            static_cast<int>(maximumFrames)},
                                           maximumFrames, block.time, done, heldEvents.data(), heldEventCount);
                held = 0;
                heldEventCount = 0;
                if (!take(status, maximumFrames, heldInChannels.data(), static_cast<int>(heldInChannels.size()), 0,
                          heldOutChannels.data(), static_cast<int>(heldOutChannels.size()), 0))
                    return false;
            }
        }
        report(block);
        return true;
    }

    // The values its processing reports, as a meter's: every parameter the host can't set, read after the block and
    // reported at its last frame where it changed.
    void report(PluginBlock& block) const
    {
        block.reportedCount = 0;
        const auto* const now = ranges.load();
        if (now == nullptr || block.reported == nullptr)
            return;
        for (const auto& range : *now)
        {
            if (range.writable)
                continue;
            if (block.reportedCount >= block.reportedRoom)
                break;
            AudioUnitParameterValue value = 0;
            if (AudioUnitGetParameter(unit, range.id, kAudioUnitScope_Global, 0, &value) != noErr ||
                value == range.reported)
                continue;
            range.reported = value;
            block.reported[block.reportedCount++] = {range.id, std::max(0, block.frames - 1), normalized(range, value)};
        }
    }

    // The buffer for a unit fed full blocks emptied, the changes gathered for its next block kept, from its start.
    void clearHeld()
    {
        for (auto& channel : heldIn)
            std::fill(channel.begin(), channel.end(), 0.0f);
        for (auto& channel : heldOut)
            std::fill(channel.begin(), channel.end(), 0.0f);
        held = 0;
        for (UInt32 i = 0; i < heldEventCount; ++i)
            heldEvents[i].eventValues.immediate.bufferOffset = 0;
    }

    // A change gathered for the next full block; with no room left, the last one gathered of the same parameter takes
    // its value, or with none, the value is set at once.
    void holdChange(const ParameterRange& range, double value, UInt32 frame)
    {
        if (heldEventCount < heldEvents.size())
        {
            heldEvents[heldEventCount++] = eventOf(range, value, frame);
            return;
        }
        for (auto i = heldEventCount; i-- > 0;)
            if (heldEvents[i].parameter == range.id)
            {
                heldEvents[i].eventValues.immediate.value = unitValue(range, value);
                return;
            }
        AudioUnitSetParameter(unit, range.id, kAudioUnitScope_Global, 0, unitValue(range, value), 0);
    }

    static AudioUnitParameterEvent eventOf(const ParameterRange& range, double value, UInt32 frame)
    {
        AudioUnitParameterEvent event{};
        event.scope = kAudioUnitScope_Global;
        event.element = 0;
        event.parameter = range.id;
        event.eventType = kParameterEvent_Immediate;
        event.eventValues.immediate.bufferOffset = frame;
        event.eventValues.immediate.value = unitValue(range, value);
        return event;
    }

    const ParameterRange* rangeOf(AudioUnitParameterID id) const
    {
        const auto* const now = ranges.load();
        if (now == nullptr)
            return nullptr;
        const auto found =
            std::lower_bound(now->begin(), now->end(), id,
                             [](const ParameterRange& range, AudioUnitParameterID one) { return range.id < one; });
        return found != now->end() && found->id == id ? &*found : nullptr;
    }

    // Its state as the unit gives it, its document form where asked for and it has one; owned by the caller, or null.
    CFPropertyListRef stateOf(bool document, OSStatus& status) const
    {
        CFPropertyListRef info = nullptr;
        UInt32 size = sizeof info;
        status = kAudioUnitErr_InvalidProperty;
        if (document)
            status = AudioUnitGetProperty(unit, kAudioUnitProperty_ClassInfoFromDocument, kAudioUnitScope_Global, 0,
                                          &info, &size);
        if (status != noErr || info == nullptr)
        {
            info = nullptr;
            size = sizeof info;
            status = AudioUnitGetProperty(unit, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0, &info, &size);
        }
        return status == noErr ? info : nullptr;
    }

    // The keys of a copy's state: the unit's own, and each value it can be set to by its ID.
    static CFStringRef ownKey() { return CFSTR("SymbolEngine state"); }
    static CFStringRef valuesKey() { return CFSTR("SymbolEngine values"); }

    // The unit's state with every value it can be set to, taking the state given.
    CFPropertyListRef withValues(CFPropertyListRef info) const
    {
        const auto values =
            CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        if (const auto* const now = ranges.load())
            for (const auto& range : *now)
            {
                AudioUnitParameterValue value = 0;
                if (!range.writable ||
                    AudioUnitGetParameter(unit, range.id, kAudioUnitScope_Global, 0, &value) != noErr)
                    continue;
                const auto key = CFStringCreateWithFormat(nullptr, nullptr, CFSTR("%u"), range.id);
                const auto number = CFNumberCreate(nullptr, kCFNumberFloat32Type, &value);
                CFDictionarySetValue(values, key, number);
                CFRelease(key);
                CFRelease(number);
            }
        const auto both =
            CFDictionaryCreateMutable(nullptr, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFDictionarySetValue(both, ownKey(), info);
        CFDictionarySetValue(both, valuesKey(), values);
        CFRelease(values);
        CFRelease(info);
        return both;
    }

    // The unit's own part of a state given, and a copy's values; neither owned.
    static CFPropertyListRef ownPart(CFPropertyListRef given, CFDictionaryRef* values = nullptr)
    {
        if (CFGetTypeID(given) != CFDictionaryGetTypeID())
            return given;
        const auto both = static_cast<CFDictionaryRef>(given);
        const auto own = CFDictionaryGetValue(both, ownKey());
        if (own == nullptr)
            return given;
        const auto found = CFDictionaryGetValue(both, valuesKey());
        if (values != nullptr && found != nullptr && CFGetTypeID(found) == CFDictionaryGetTypeID())
            *values = static_cast<CFDictionaryRef>(found);
        return own;
    }

    // Gives a state, a document's as one first, then a copy's values.
    OSStatus give(CFPropertyListRef given, bool asDocument)
    {
        CFDictionaryRef values = nullptr;
        const auto own = ownPart(given, &values);
        OSStatus status = kAudioUnitErr_InvalidProperty;
        if (asDocument)
            status = AudioUnitSetProperty(unit, kAudioUnitProperty_ClassInfoFromDocument, kAudioUnitScope_Global, 0,
                                          &own, sizeof own);
        if (status != noErr)
            status =
                AudioUnitSetProperty(unit, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0, &own, sizeof own);
        if (status == noErr && values != nullptr)
            CFDictionaryApplyFunction(
                values,
                [](const void* key, const void* value, void* context) {
                    float number = 0;
                    if (CFGetTypeID(value) != CFNumberGetTypeID() ||
                        !CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberFloat32Type, &number))
                        return;
                    const auto id = static_cast<AudioUnitParameterID>(std::strtoul(textOf(key).c_str(), nullptr, 10));
                    AudioUnitSetParameter(static_cast<AudioUnitPlugin*>(context)->unit, id, kAudioUnitScope_Global, 0,
                                          number, 0);
                },
                this);
        return status;
    }

    // Its listeners, its own window's among them, told every value changed, and the host told to read them again; the
    // host's own listener left out, as the host reads them itself.
    void valuesChanged()
    {
        AudioUnitParameter every{unit, kAUParameterListener_AnyParameter, kAudioUnitScope_Global, 0};
        AUParameterListenerNotify(listener, nullptr, &every);
        client.notice(PluginNotice::values);
    }

    void set(AudioUnitParameterID id, double value)
    {
        if (const auto* const range = rangeOf(id); range != nullptr && range->writable)
            AudioUnitSetParameter(unit, id, kAudioUnitScope_Global, 0, unitValue(*range, value), 0);
    }

    std::vector<AudioUnitParameterID> parameterIds() const
    {
        UInt32 size = 0;
        if (AudioUnitGetPropertyInfo(unit, kAudioUnitProperty_ParameterList, kAudioUnitScope_Global, 0, &size,
                                     nullptr) != noErr ||
            size == 0)
            return {};
        std::vector<AudioUnitParameterID> ids(size / sizeof(AudioUnitParameterID));
        if (AudioUnitGetProperty(unit, kAudioUnitProperty_ParameterList, kAudioUnitScope_Global, 0, ids.data(),
                                 &size) != noErr)
            return {};
        ids.resize(size / sizeof(AudioUnitParameterID));
        return ids;
    }

    // Listens for the gestures and values of exactly these parameters, as a listener can't ask for every one.
    void listenTo(const std::vector<PluginParameter>& parameters)
    {
        if (listener == nullptr)
            return;
        constexpr AudioUnitEventType kinds[] = {kAudioUnitEvent_BeginParameterChangeGesture,
                                                kAudioUnitEvent_ParameterValueChange,
                                                kAudioUnitEvent_EndParameterChangeGesture};
        const auto each = [&](AudioUnitParameterID id, auto change) {
            for (const auto kind : kinds)
            {
                AudioUnitEvent event{};
                event.mEventType = kind;
                event.mArgument.mParameter = {unit, id, kAudioUnitScope_Global, 0};
                change(listener, nullptr, &event);
            }
        };
        for (const auto id : listened)
            each(id, AUEventListenerRemoveEventType);
        listened.clear();
        for (const auto& parameter : parameters)
        {
            each(parameter.id, AUEventListenerAddEventType);
            listened.push_back(parameter.id);
        }
    }

    // An edit of its own, on the listener's queue.
    void heard(const AudioUnitEvent& event, AudioUnitParameterValue value)
    {
        const auto id = event.mArgument.mParameter.mParameterID;
        if (event.mEventType == kAudioUnitEvent_BeginParameterChangeGesture)
            client.edit({PluginEdit::Kind::begin, id, 0.0});
        else if (event.mEventType == kAudioUnitEvent_EndParameterChangeGesture)
            client.edit({PluginEdit::Kind::end, id, 0.0});
        else if (const auto* const range = rangeOf(id);
                 event.mEventType == kAudioUnitEvent_ParameterValueChange && range != nullptr)
            client.edit({PluginEdit::Kind::value, id, normalized(*range, value)});
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
        auto& client = static_cast<AudioUnitPlugin*>(context)->client;
        if (id == kAudioUnitProperty_Latency)
            client.notice(PluginNotice::latency);
        else if (id == kAudioUnitProperty_ParameterList)
            client.notice(PluginNotice::parameters);
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

// A unit's Cocoa view in the program's: made from the class the bundle its description names holds, on an initialized
// unit, every exception from making it caught; put in the program's view once that is sized to it, at its top left;
// and followed as it sizes itself, the program's view resized to it with the view's own sizing held still meanwhile,
// so the window never feeds back, a size of none taken as not yet known and an origin it moves put back. It is taken
// out, never hidden first, and let go of before the unit is uninitialized, and put back once it is initialized again.
class AudioUnitEditor final : public PluginFormatEditor
{
public:
    AudioUnitEditor(AudioUnitPlugin& owner, void* parentView, PluginEditorFrame& holder)
        : plugin(owner), parent(static_cast<NSView*>(parentView)), frame(holder)
    {
        plugin.editor = this;
    }

    ~AudioUnitEditor() override
    {
        takeOut();
        plugin.editor = nullptr;
    }

    // Its view made and put in, or false with the error set.
    bool putIn(PluginError& error)
    {
        NSString* className = nil;
        std::string location;

        if (!describe(className, location, error))
            return false;

        @try
        {
            // Only from the bundle its description names, as two plugins' classes can share a name in one process.
            NSBundle* bundle = [NSBundle bundleWithPath:[NSString stringWithUTF8String:location.c_str()]];
            Class found = bundle != nil ? [bundle classNamed:className] : nil;
            const auto ownBundle = [&](Class made) {
                return [[NSBundle bundleForClass:made].bundlePath.stringByStandardizingPath
                    isEqualToString:bundle.bundlePath.stringByStandardizingPath];
            };

            if (found == nil && NSClassFromString(className) == nil)
            {
                error = {"Its editor's class isn't in its bundle.", "openEditor", 0};
                return false;
            }

            if (found == nil || !ownBundle(found))
            {
                error = {"Its editor's class collides with another plugin's.", "openEditor", 0};
                return false;
            }

            if (![found conformsToProtocol:@protocol(AUCocoaUIBase)] ||
                ![found instancesRespondToSelector:@selector(uiViewForAudioUnit:withSize:)])
            {
                error = {"Its editor's class makes no Audio Unit view.", "openEditor", 0};
                return false;
            }

            factory = [[found alloc] init];
            NSView* made =
                [static_cast<id<AUCocoaUIBase>>(factory) uiViewForAudioUnit:plugin.handle() withSize:NSZeroSize];

            if (made == nil)
            {
                error = {"Its editor gave no view.", "openEditor", 0};
                [factory release];
                factory = nil;
                return false;
            }

            view = [made retain];
        }
        @catch (NSException* exception)
        {
            const char* why = exception.reason != nil ? exception.reason.UTF8String : nullptr;
            error = {std::string("Its editor couldn't be made: ") + (why != nullptr ? why : "an exception") + ".",
                     "openEditor", 0};
            [view release];
            view = nil;
            [factory release];
            factory = nil;
            return false;
        }

        // The program's view sized to it first, unless it has no size yet; then it, at its top left.
        if (const auto own = sizeOf(view.frame.size); own.width > 0 && own.height > 0)
        {
            current = own;
            place(current);
        }

        placing = true;
        view.postsFrameChangedNotifications = YES;
        [parent addSubview:view];
        toTopLeft();
        placing = false;

        // Its own frame followed, and kept at the top left as the program's view changes, as when it is settled.
        auto* self = this;
        auto* center = [NSNotificationCenter defaultCenter];
        parent.postsFrameChangedNotifications = YES;
        watcher = [[center addObserverForName:NSViewFrameDidChangeNotification
                                       object:view
                                        queue:nil
                                   usingBlock:^(NSNotification*) {
                                     self->followed();
                                   }] retain];
        parentWatcher = [[center addObserverForName:NSViewFrameDidChangeNotification
                                             object:parent
                                              queue:nil
                                         usingBlock:^(NSNotification*) {
                                           self->kept();
                                         }] retain];
        return true;
    }

    // Its view taken out and let go of; the program's view stays as it is.
    void takeOut()
    {
        if (view == nil)
            return;

        for (id* one : {&watcher, &parentWatcher})
        {
            [[NSNotificationCenter defaultCenter] removeObserver:*one];
            [*one release];
            *one = nil;
        }

        @try
        {
            [view removeFromSuperview];
        }
        @catch (NSException*)
        {
        }

        [view release];
        view = nil;
        [factory release];
        factory = nil;
    }

    PluginEditorSize size() override { return current; }

    // Resizable only if its view is.
    bool resizable() override
    {
        return view != nil && (view.autoresizingMask & (NSViewWidthSizable | NSViewHeightSizable)) != 0;
    }

    PluginEditorSize constrain(PluginEditorSize dragged) override { return resizable() ? dragged : current; }

    void resized(PluginEditorSize size) override
    {
        if (view == nil || !resizable() || size == current)
            return;

        current = size;
        placing = true;
        [view setFrameSize:NSMakeSize(size.width, size.height)];
        toTopLeft();
        placing = false;
    }

private:
    static PluginEditorSize sizeOf(NSSize size)
    {
        return {static_cast<int>(std::lround(size.width)), static_cast<int>(std::lround(size.height))};
    }

    // Its description, as the unit gives it: the file path of the bundle holding its class, never the URL's text,
    // which escapes what the path holds, and the class's name; the strings and URL let go of as the host owns them.
    bool describe(NSString*& className, std::string& location, PluginError& error)
    {
        UInt32 size = 0;
        Boolean writable = false;
        const auto unit = plugin.handle();

        if (AudioUnitGetPropertyInfo(unit, kAudioUnitProperty_CocoaUI, kAudioUnitScope_Global, 0, &size, &writable) !=
                noErr ||
            size < sizeof(AudioUnitCocoaViewInfo))
        {
            error = {"It has no editor.", "openEditor", 0};
            return false;
        }

        std::vector<std::byte> room(size);
        UInt32 given = size;

        if (AudioUnitGetProperty(unit, kAudioUnitProperty_CocoaUI, kAudioUnitScope_Global, 0, room.data(), &given) !=
                noErr ||
            given < sizeof(AudioUnitCocoaViewInfo) || given > size)
        {
            error = {"Its editor's description can't be read.", "openEditor", 0};
            return false;
        }

        auto* info = reinterpret_cast<AudioUnitCocoaViewInfo*>(room.data());
        const auto classes = (given - offsetof(AudioUnitCocoaViewInfo, mCocoaAUViewClass)) / sizeof(CFStringRef);
        char path[PATH_MAX] = {};

        if (info->mCocoaAUViewBundleLocation != nullptr &&
            CFURLGetFileSystemRepresentation(info->mCocoaAUViewBundleLocation, true, reinterpret_cast<UInt8*>(path),
                                             sizeof path))
            location = path;

        if (info->mCocoaAUViewClass[0] != nullptr)
            className = [[static_cast<NSString*>(info->mCocoaAUViewClass[0]) copy] autorelease];

        if (info->mCocoaAUViewBundleLocation != nullptr)
            CFRelease(info->mCocoaAUViewBundleLocation);

        for (std::size_t c = 0; c < classes; ++c)
            if (info->mCocoaAUViewClass[c] != nullptr)
                CFRelease(info->mCocoaAUViewClass[c]);

        if (location.empty() || className == nil)
        {
            error = {"Its editor's description can't be read.", "openEditor", 0};
            return false;
        }

        return true;
    }

    // The program's view resized to the size, the view's own sizing held still meanwhile so it isn't fed back.
    void place(PluginEditorSize size)
    {
        const auto mask = view.autoresizingMask;
        view.autoresizingMask = NSViewNotSizable;
        placing = true;
        frame.place(size);
        toTopLeft();
        placing = false;
        view.autoresizingMask = mask;
    }

    void toTopLeft()
    {
        const auto origin = NSMakePoint(0, NSHeight(parent.bounds) - NSHeight(view.frame));

        if (!NSEqualPoints(view.frame.origin, origin))
            [view setFrameOrigin:origin];
    }

    // The view's frame changed by the view itself: a size it gives followed, and an origin it moved put back.
    void followed()
    {
        if (placing || view == nil)
            return;

        if (const auto own = sizeOf(view.frame.size); own.width > 0 && own.height > 0 && own != current)
        {
            current = own;
            place(current);
            return;
        }

        placing = true;
        toTopLeft();
        placing = false;
    }

    void kept()
    {
        if (placing || view == nil)
            return;

        placing = true;
        toTopLeft();
        placing = false;
    }

    AudioUnitPlugin& plugin;
    NSView* parent;
    PluginEditorFrame& frame;
    NSView* view = nil;
    id factory = nil;
    id watcher = nil;
    id parentWatcher = nil;
    bool placing = false;
    PluginEditorSize current{};
};

std::unique_ptr<PluginFormatEditor> AudioUnitPlugin::openEditor(void* parent, PluginEditorFrame& frame,
                                                                PluginError& error)
{
    if (!initialized)
    {
        error = {"An Audio Unit's editor opens once it is prepared.", "openEditor", 0};
        return nullptr;
    }

    if (editor != nullptr)
    {
        error = {"Its editor is open already.", "openEditor", 0};
        return nullptr;
    }

    auto made = std::make_unique<AudioUnitEditor>(*this, parent, frame);

    if (!made->putIn(error))
        return nullptr;

    return made;
}

void AudioUnitPlugin::viewOut()
{
    if (editor != nullptr)
        editor->takeOut();
}

void AudioUnitPlugin::viewBack()
{
    if (PluginError ignored; editor != nullptr)
        editor->putIn(ignored);
}

}

struct AudioUnitFormat::Shared
{
    Options options;
};

namespace
{

// Every format told of the system's notice that its registry changed, through one observer for the process, as the
// system may post it on any thread while a format is being taken down; each told on its main thread while it lives.
struct RegistryWatch
{
    std::mutex lock;
    std::vector<std::weak_ptr<AudioUnitFormat::Shared>> formats;
};

void registryChanged(CFNotificationCenterRef, void* observer, CFNotificationName, const void*, CFDictionaryRef)
{
    auto& watch = *static_cast<RegistryWatch*>(observer);
    std::vector<std::shared_ptr<AudioUnitFormat::Shared>> live;
    {
        const std::lock_guard held(watch.lock);
        for (const auto& format : watch.formats)
            if (auto shared = format.lock())
                live.push_back(std::move(shared));
    }
    for (const auto& shared : live)
        if (shared->options.registryChanged != nullptr && shared->options.post != nullptr)
            shared->options.post([weak = std::weak_ptr(shared)] {
                if (const auto alive = weak.lock())
                    alive->options.registryChanged();
            });
}

// Made once and kept for the life of the process, as the system can call its observer at any moment.
RegistryWatch& registryWatch()
{
    static auto* const watch = [] {
        auto* const made = new RegistryWatch;
        CFNotificationCenterAddObserver(CFNotificationCenterGetLocalCenter(), made, &registryChanged,
                                        kAudioComponentRegistrationsChangedNotification, nullptr,
                                        CFNotificationSuspensionBehaviorDeliverImmediately);
        return made;
    }();
    return *watch;
}

// The system's component for a unit's codes, of version 2: a unit of version 3 standing in under the same codes isn't
// made as it.
AudioComponent componentFor(const PluginIdentity& identity)
{
    const auto description = descriptionOf(identity);
    if (!description)
        return nullptr;
    for (auto component = AudioComponentFindNext(nullptr, &*description); component != nullptr;
         component = AudioComponentFindNext(component, &*description))
    {
        AudioComponentDescription found{};
        if (AudioComponentGetDescription(component, &found) == noErr &&
            (found.componentFlags & kAudioComponentFlag_IsV3AudioUnit) == 0)
            return component;
    }
    return nullptr;
}

}

AudioUnitFormat::AudioUnitFormat(Options options) : shared(std::make_shared<Shared>())
{
    shared->options = std::move(options);
    auto& watch = registryWatch();
    const std::lock_guard held(watch.lock);
    watch.formats.push_back(shared);
}

AudioUnitFormat::~AudioUnitFormat()
{
    auto& watch = registryWatch();
    const std::lock_guard held(watch.lock);
    std::erase_if(watch.formats, [this](const std::weak_ptr<Shared>& format) {
        const auto alive = format.lock();
        return alive == nullptr || alive == shared;
    });
}

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

bool AudioUnitFormat::runs(std::string_view architecture) const
{
    return PluginFormat::runs(architecture) || (system::architecture() == "arm64" && architecture == "x86_64");
}

// The binary a unit's Info.plist names, or nothing where it names none.
static std::filesystem::path binaryNamedBy(const std::filesystem::path& file)
{
    const auto info = infoOf(file);
    const auto executable = info != nullptr ? textOf(CFDictionaryGetValue(info, CFSTR("CFBundleExecutable"))) : "";
    if (info != nullptr)
        CFRelease(info);
    return executable.empty() ? std::filesystem::path{} : file / "Contents" / "MacOS" / paths::fromUtf8(executable);
}

std::optional<std::filesystem::path> AudioUnitFormat::changedAfterSigning(const std::filesystem::path& file) const
{
    const auto binary = binaryNamedBy(file);
    return binary.empty() ? std::nullopt : pluginFiles::changedAfterSigning(binary);
}

std::optional<std::filesystem::path> AudioUnitFormat::notNotarized(const std::filesystem::path& file) const
{
    const auto binary = binaryNamedBy(file);
    return binary.empty() ? std::nullopt : pluginFiles::notNotarized(binary);
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

bool AudioUnitFormat::registered(const PluginDescription& plugin) const
{
    return componentFor(plugin.identity) != nullptr;
}

void AudioUnitFormat::make(const PluginDescription& plugin, PluginFormatClient& client,
                           std::function<void(std::unique_ptr<PluginFormatInstance>, PluginError)> done)
{
    const auto component = componentFor(plugin.identity);
    if (component == nullptr)
    {
        done(nullptr, {"It isn't among the Audio Units the system knows.", "make", 0});
        return;
    }

    // Made without blocking the main thread, its completion handed to the main thread whatever thread it comes on; in
    // the program's own process, except a unit built only for Intel on Apple Silicon, which the system runs in a
    // process of its own through Rosetta for as long as macOS has it.
    PluginError unread;
    const auto facts = plugin.location.empty() ? std::nullopt : examine(plugin.location, unread);
    const auto builtFor = [&facts](std::string_view architecture) {
        return facts && std::find(facts->architectures.begin(), facts->architectures.end(), architecture) !=
                            facts->architectures.end();
    };
    const bool throughRosetta =
        system::architecture() == "arm64" && builtFor("x86_64") && !builtFor("arm64") && !builtFor("arm64e");
    auto post = shared->options.post;
    auto* const told = &client;
    AudioComponentInstantiate(component, throughRosetta ? kAudioComponentInstantiation_LoadOutOfProcess : 0,
                              ^(AudioComponentInstance instance, OSStatus status) {
                                post([instance, status, done, told] {
                                    if (status != noErr || instance == nullptr)
                                        done(nullptr, {reason("It couldn't be made", status), "make", status});
                                    else
                                        done(std::make_unique<AudioUnitPlugin>(instance, *told), {});
                                });
                              });
}

}
