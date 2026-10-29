// The host's test Audio Units, built on Apple's AudioUnitSDK, one component holding many. Each checks the calls a host
// makes into it: everything but rendering on the main thread, nothing overlapping a render, its largest block and every
// bus's format set before it is initialized; and writes what was wrong to the file SYMBOLENGINE_TEST_AU_LOG names. Most
// also misbehave one way, as units do.

#include <AudioUnitSDK/AUEffectBase.h>

#include <AudioToolbox/AudioUnitUtilities.h>

#include <mach/mach_time.h>
#include <pthread.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

namespace
{

void misuse(const char* unit, const std::string& what)
{
    static std::mutex lock;
    const std::lock_guard held(lock);
    if (const char* path = std::getenv("SYMBOLENGINE_TEST_AU_LOG"))
        if (FILE* log = std::fopen(path, "ab"))
        {
            std::fprintf(log, "%s: %s\n", unit, what.c_str());
            std::fclose(log);
        }
}

// The checks every test unit makes; it passes its input through unless it says otherwise.
class Checked : public ausdk::AUEffectBase
{
public:
    Checked(AudioComponentInstance instance, const char* unitName) : AUEffectBase(instance, false), name(unitName) {}

    OSStatus Initialize() override
    {
        onMain("Initialize");
        if (!largestSet)
            misuse(name, "initialized before its largest block was set");
        if (!inputSet || !outputSet)
            misuse(name, "initialized before every bus's format was set");
        return AUEffectBase::Initialize();
    }

    void Cleanup() override
    {
        onMain("Cleanup");
        if (rendering)
            misuse(name, "uninitialized while rendering");
        AUEffectBase::Cleanup();
    }

    OSStatus Reset(AudioUnitScope scope, AudioUnitElement element) override
    {
        onMain("Reset");
        if (rendering)
            misuse(name, "reset while rendering");
        return AUEffectBase::Reset(scope, element);
    }

    void SetMaxFramesPerSlice(UInt32 frames) override
    {
        onMain("SetMaxFramesPerSlice");
        largestSet = constructed;
        AUEffectBase::SetMaxFramesPerSlice(frames);
    }

    // Only what the host sets from here on counts, as the SDK sets a default as it makes the unit.
    void PostConstructor() override
    {
        AUEffectBase::PostConstructor();
        constructed = true;
        largestSet = false;
    }

    OSStatus ChangeStreamFormat(AudioUnitScope scope, AudioUnitElement element, const AudioStreamBasicDescription& from,
                                const AudioStreamBasicDescription& to) override
    {
        onMain("ChangeStreamFormat");
        noteFormatSet(scope);
        return AUEffectBase::ChangeStreamFormat(scope, element, from, to);
    }

    OSStatus SetProperty(AudioUnitPropertyID id, AudioUnitScope scope, AudioUnitElement element, const void* data,
                         UInt32 size) override
    {
        if (id == kAudioUnitProperty_StreamFormat)
            noteFormatSet(scope);
        return AUEffectBase::SetProperty(id, scope, element, data, size);
    }

    // A host's value never comes from another thread while the unit renders, as some units lock their render against
    // that.
    OSStatus SetParameter(AudioUnitParameterID id, AudioUnitScope scope, AudioUnitElement element,
                          AudioUnitParameterValue value, UInt32 offset) override
    {
        if (rendering && pthread_equal(pthread_self(), renderer.load()) == 0)
            misuse(name, "a parameter set from another thread while it rendered");
        return AUEffectBase::SetParameter(id, scope, element, value, offset);
    }

    OSStatus Render(AudioUnitRenderActionFlags& flags, const AudioTimeStamp& time, UInt32 frames) override
    {
        renderer = pthread_self();
        if (rendering.exchange(true))
            misuse(name, "rendered while already rendering");
        if (pthread_main_np() != 0)
            misuse(name, "rendered on the main thread");
        if (!IsInitialized())
            misuse(name, "rendered uninitialized");
        if (frames > GetMaxFramesPerSlice())
            misuse(name, "given " + std::to_string(frames) + " frames, more than its largest block of " +
                             std::to_string(GetMaxFramesPerSlice()));
        const auto result = render(flags, time, frames);
        rendering = false;
        return result;
    }

    OSStatus ProcessBufferLists(AudioUnitRenderActionFlags& flags, const AudioBufferList& in, AudioBufferList& out,
                                UInt32 frames) override
    {
        return process(flags, in, out, frames);
    }

    bool SupportsTail() override { return true; }

protected:
    const char* name;
    std::atomic<bool> rendering{false};
    std::atomic<pthread_t> renderer{};

    virtual OSStatus render(AudioUnitRenderActionFlags& flags, const AudioTimeStamp& time, UInt32 frames)
    {
        return AUEffectBase::Render(flags, time, frames);
    }

    virtual OSStatus process(AudioUnitRenderActionFlags&, const AudioBufferList& in, AudioBufferList& out,
                             UInt32 frames)
    {
        for (UInt32 c = 0; c < std::min(in.mNumberBuffers, out.mNumberBuffers); ++c)
            std::copy_n(static_cast<const float*>(in.mBuffers[c].mData), frames,
                        static_cast<float*>(out.mBuffers[c].mData));
        return noErr;
    }

    bool largestBlockSet() const { return largestSet; }

    void onMain(const char* call) const
    {
        if (pthread_main_np() == 0)
            misuse(name, std::string(call) + " called off the main thread");
    }

private:
    bool constructed = false;
    bool largestSet = false;
    bool inputSet = false;
    bool outputSet = false;

    void noteFormatSet(AudioUnitScope scope) { (scope == kAudioUnitScope_Input ? inputSet : outputSet) = true; }
};

// A unit that takes each change scheduled for a render at its frame, as Apple's SDK sets an immediate change's value
// as it is scheduled, before the render: a gain from 0 to 2, parameter 0 unless it says otherwise, which it applies to
// its input.
class Scheduled : public Checked
{
public:
    Scheduled(AudioComponentInstance instance, const char* unitName) : Checked(instance, unitName)
    {
        Globals()->UseIndexedParameters(1);
        Globals()->SetParameter(0, 1.0f);
    }

    OSStatus GetParameterInfo(AudioUnitScope scope, AudioUnitParameterID id, AudioUnitParameterInfo& info) override
    {
        if (scope != kAudioUnitScope_Global || id != 0)
            return kAudioUnitErr_InvalidParameter;
        FillInParameterName(info, CFSTR("Gain"), false);
        info.flags = kAudioUnitParameterFlag_IsReadable | kAudioUnitParameterFlag_IsWritable;
        info.unit = kAudioUnitParameterUnit_LinearGain;
        info.minValue = 0;
        info.maxValue = 2;
        info.defaultValue = 1;
        return noErr;
    }

    OSStatus ScheduleParameter(const AudioUnitParameterEvent* changes, UInt32 count) override
    {
        for (UInt32 i = 0; i < count; ++i)
            if (changes[i].eventType != kParameterEvent_Immediate || waiting == pending.size())
                misuse(name, "given a change it can't keep");
            else
                pending[waiting++] = changes[i];
        return noErr;
    }

    // A render's changes are for that render alone, whether or not it rendered them.
    OSStatus Render(AudioUnitRenderActionFlags& flags, const AudioTimeStamp& time, UInt32 frames) override
    {
        const auto result = Checked::Render(flags, time, frames);
        waiting = 0;
        return result;
    }

protected:
    OSStatus process(AudioUnitRenderActionFlags&, const AudioBufferList& in, AudioBufferList& out,
                     UInt32 frames) override
    {
        std::stable_sort(pending.begin(), pending.begin() + waiting, [](const auto& a, const auto& b) {
            return a.eventValues.immediate.bufferOffset < b.eventValues.immediate.bufferOffset;
        });
        std::size_t next = 0;
        for (UInt32 i = 0; i < frames; ++i)
        {
            for (; next < waiting && pending[next].eventValues.immediate.bufferOffset <= i; ++next)
                SetParameter(pending[next].parameter, pending[next].scope, pending[next].element,
                             pending[next].eventValues.immediate.value, 0);
            const auto level = Globals()->GetParameter(0);
            for (UInt32 c = 0; c < std::min(in.mNumberBuffers, out.mNumberBuffers); ++c)
                static_cast<float*>(out.mBuffers[c].mData)[i] =
                    static_cast<const float*>(in.mBuffers[c].mData)[i] * level;
        }
        for (; next < waiting; ++next)
            misuse(name, "given a change past its render");
        return noErr;
    }

private:
    std::array<AudioUnitParameterEvent, 64> pending{};
    std::size_t waiting = 0;
};

// Passes its input through, with a latency it says only once initialized.
class Pass final : public Checked
{
public:
    explicit Pass(AudioComponentInstance instance) : Checked(instance, "Pass") {}
    Float64 GetLatency() override { return IsInitialized() ? 0.010 : 0.0; }
};

// Fails every tenth render, after filling its output with a pattern no host should play.
class Fail final : public Checked
{
public:
    explicit Fail(AudioComponentInstance instance) : Checked(instance, "Fail") {}

protected:
    OSStatus process(AudioUnitRenderActionFlags& flags, const AudioBufferList& in, AudioBufferList& out,
                     UInt32 frames) override
    {
        if (++renders % 10 != 0)
            return Checked::process(flags, in, out, frames);
        for (UInt32 c = 0; c < out.mNumberBuffers; ++c)
            std::fill_n(static_cast<float*>(out.mBuffers[c].mData), frames, 0.777f);
        return kAudioUnitErr_Unauthorized;
    }

private:
    int renders = 0;
};

// Says its instance is gone from its hundredth render on, as an extension whose process crashed does.
class Dead final : public Checked
{
public:
    explicit Dead(AudioComponentInstance instance) : Checked(instance, "Dead") {}

protected:
    OSStatus process(AudioUnitRenderActionFlags& flags, const AudioBufferList& in, AudioBufferList& out,
                     UInt32 frames) override
    {
        if (++renders < 100)
            return Checked::process(flags, in, out, frames);
        if (renders > 100)
            misuse(name, "rendered after it said its instance was gone");
        return kAudioComponentErr_InstanceInvalidated;
    }

private:
    int renders = 0;
};

// Takes no largest block above the SDK's default of 1,156 frames, and halves its input, so a block that never
// reached it shows.
class Small final : public Checked
{
public:
    explicit Small(AudioComponentInstance instance) : Checked(instance, "Small") {}
    void SetMaxFramesPerSlice(UInt32 frames) override
    {
        Checked::SetMaxFramesPerSlice(std::min<UInt32>(frames, kAUDefaultMaxFramesPerSlice));
    }

protected:
    OSStatus process(AudioUnitRenderActionFlags&, const AudioBufferList& in, AudioBufferList& out,
                     UInt32 frames) override
    {
        for (UInt32 c = 0; c < std::min(in.mNumberBuffers, out.mNumberBuffers); ++c)
            for (UInt32 i = 0; i < frames; ++i)
                static_cast<float*>(out.mBuffers[c].mData)[i] =
                    static_cast<const float*>(in.mBuffers[c].mData)[i] * 0.5f;
        return noErr;
    }
};

// Pulls its input in three pieces, each to go on where the last left off, and then seven frames more past the block,
// which are to be silence.
class Pull final : public Checked
{
public:
    explicit Pull(AudioComponentInstance instance) : Checked(instance, "Pull") {}

protected:
    OSStatus render(AudioUnitRenderActionFlags& flags, const AudioTimeStamp& time, UInt32 frames) override
    {
        auto& out = Output(0).PrepareBuffer(frames);
        AudioTimeStamp at = time;
        UInt32 done = 0;
        for (const UInt32 piece : {frames / 3, frames / 3, frames - 2 * (frames / 3)})
        {
            if (const auto result = Input(0).PullInput(flags, at, 0, piece); result != noErr)
                return result;
            const auto& in = Input(0).GetBufferList();
            for (UInt32 c = 0; c < std::min(in.mNumberBuffers, out.mNumberBuffers); ++c)
                std::copy_n(static_cast<const float*>(in.mBuffers[c].mData), piece,
                            static_cast<float*>(out.mBuffers[c].mData) + done);
            done += piece;
            at.mSampleTime += piece;
        }
        if (Input(0).PullInput(flags, at, 0, 7) == noErr)
        {
            const auto& past = Input(0).GetBufferList();
            for (UInt32 c = 0; c < past.mNumberBuffers; ++c)
                for (UInt32 i = 0; i < 7; ++i)
                    if (static_cast<const float*>(past.mBuffers[c].mData)[i] != 0.0f)
                        misuse(name, "given sound past the end of the block");
        }
        return noErr;
    }
};

// Hands back its first channel in a buffer of its own, and leaves its second unwritten.
class Swap final : public Checked
{
public:
    explicit Swap(AudioComponentInstance instance) : Checked(instance, "Swap") {}

protected:
    OSStatus process(AudioUnitRenderActionFlags&, const AudioBufferList& in, AudioBufferList& out,
                     UInt32 frames) override
    {
        own.resize(std::max<std::size_t>(own.size(), frames));
        std::copy_n(static_cast<const float*>(in.mBuffers[0].mData), frames, own.data());
        out.mBuffers[0].mData = own.data();
        if (out.mNumberBuffers > 1)
        {
            std::fill_n(static_cast<float*>(out.mBuffers[1].mData), frames, 0.5f);
            out.mBuffers[1].mDataByteSize = 0;
        }
        return noErr;
    }

private:
    std::vector<float> own;
};

// A delay of 100 frames that says its output is silent whenever its input is, echoes or not.
class Echo final : public Checked
{
public:
    explicit Echo(AudioComponentInstance instance) : Checked(instance, "Echo") {}

    OSStatus Reset(AudioUnitScope scope, AudioUnitElement element) override
    {
        for (auto& line : lines)
            std::fill(line.begin(), line.end(), 0.0f);
        return Checked::Reset(scope, element);
    }

protected:
    // Says its output is silent after the SDK's own render, which would otherwise zero its output itself.
    OSStatus render(AudioUnitRenderActionFlags& flags, const AudioTimeStamp& time, UInt32 frames) override
    {
        const auto result = Checked::render(flags, time, frames);
        if (silentInput)
            flags |= kAudioUnitRenderAction_OutputIsSilence;
        return result;
    }

    OSStatus process(AudioUnitRenderActionFlags&, const AudioBufferList& in, AudioBufferList& out,
                     UInt32 frames) override
    {
        bool silent = true;
        for (UInt32 c = 0; c < std::min<UInt32>(std::min(in.mNumberBuffers, out.mNumberBuffers), 2); ++c)
        {
            const auto* from = static_cast<const float*>(in.mBuffers[c].mData);
            auto* to = static_cast<float*>(out.mBuffers[c].mData);
            auto& line = lines[c];
            for (UInt32 i = 0; i < frames; ++i)
            {
                silent = silent && from[i] == 0.0f;
                to[i] = line[at[c]];
                line[at[c]] = from[i];
                at[c] = (at[c] + 1) % line.size();
            }
        }
        silentInput = silent;
        return noErr;
    }

private:
    bool silentInput = false;
    std::vector<float> lines[2]{std::vector<float>(100), std::vector<float>(100)};
    std::size_t at[2]{};
};

// Starts with ten channels on each side, taking any number.
class Ten final : public Checked
{
public:
    explicit Ten(AudioComponentInstance instance) : Checked(instance, "Ten") {}

    // Once its buses are made, after it is constructed.
    void PostConstructor() override
    {
        Checked::PostConstructor();
        AudioStreamBasicDescription format = Output(0).GetStreamFormat();
        format.mChannelsPerFrame = 10;
        Output(0).SetStreamFormat(format);
        Input(0).SetStreamFormat(format);
    }

    UInt32 SupportedNumChannels(const AUChannelInfo** info) override
    {
        static const AUChannelInfo any{-1, -1};
        if (info != nullptr)
            *info = &any;
        return 1;
    }

protected:
    OSStatus process(AudioUnitRenderActionFlags& flags, const AudioBufferList& in, AudioBufferList& out,
                     UInt32 frames) override
    {
        if (out.mNumberBuffers != 2)
            misuse(name, "rendered with " + std::to_string(out.mNumberBuffers) + " channels");
        return Checked::process(flags, in, out, frames);
    }
};

// Turns its output to mono as it is initialized, whatever it was set to.
class Grow final : public Checked
{
public:
    explicit Grow(AudioComponentInstance instance) : Checked(instance, "Grow") {}

    OSStatus Initialize() override
    {
        const auto result = Checked::Initialize();
        AudioStreamBasicDescription format = Output(0).GetStreamFormat();
        format.mChannelsPerFrame = 1;
        Output(0).SetStreamFormat(format);
        return result;
    }
};

// Asks the host's callbacks for its tempo, beat, meter and transport in every render, writing the tempo / 1,000 to its
// first channel and the beat to its second, and checks its time stamps: sample time running on by exactly the frames
// rendered, host time near the system's clock, and no callback answering outside a render.
class Time final : public Checked
{
public:
    explicit Time(AudioComponentInstance instance) : Checked(instance, "Time") {}

    OSStatus Initialize() override
    {
        if (CallHostBeatAndTempo(nullptr, nullptr) == -1)
            misuse(name, "initialized before its host callbacks were set");
        expected = -1;
        return Checked::Initialize();
    }

    OSStatus Reset(AudioUnitScope scope, AudioUnitElement element) override
    {
        Float64 beat = 0;
        Float64 tempo = 0;
        if (IsInitialized() && CallHostBeatAndTempo(&beat, &tempo) != kAudioUnitErr_CannotDoInCurrentContext)
            misuse(name, "a host callback answered outside a render");
        expected = -1;
        return Checked::Reset(scope, element);
    }

protected:
    OSStatus render(AudioUnitRenderActionFlags& flags, const AudioTimeStamp& time, UInt32 frames) override
    {
        if (expected >= 0 && time.mSampleTime != expected)
            misuse(name, "its sample time jumped from " + std::to_string(expected) + " to " +
                             std::to_string(time.mSampleTime));
        expected = time.mSampleTime + frames;
        if ((time.mFlags & kAudioTimeStampHostTimeValid) != 0)
        {
            mach_timebase_info_data_t base{};
            mach_timebase_info(&base);
            const auto now = static_cast<double>(mach_absolute_time()) * base.numer / base.denom;
            const auto then = static_cast<double>(time.mHostTime) * base.numer / base.denom;
            if (std::abs(now - then) > 250e6)
                misuse(name, "its host time is " + std::to_string((now - then) / 1e6) + " ms from the clock");
        }

        // Every callback, with every argument and with none.
        CallHostBeatAndTempo(nullptr, nullptr);
        CallHostMusicalTimeLocation(nullptr, nullptr, nullptr, nullptr);
        CallHostTransportState(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
        Float64 beat = -1;
        Float64 tempo = -1;
        UInt32 toNextBeat = 0;
        Float32 numerator = 0;
        UInt32 denominator = 0;
        Float64 downbeat = 0;
        Boolean playing = false;
        Boolean changed = false;
        Boolean cycling = true;
        Float64 sample = 0;
        Float64 cycleStart = 0;
        Float64 cycleEnd = 0;
        if (CallHostBeatAndTempo(&beat, &tempo) != noErr ||
            CallHostMusicalTimeLocation(&toNextBeat, &numerator, &denominator, &downbeat) != noErr ||
            CallHostTransportState(&playing, &changed, &sample, &cycling, &cycleStart, &cycleEnd) != noErr)
            misuse(name, "a host callback failed in a render");
        if (tempo <= 0 || numerator <= 0 || denominator == 0 || cycling)
            misuse(name, "a host callback gave a tempo, meter or cycle no host should");

        const auto result = Checked::render(flags, time, frames);
        auto& out = Output(0).GetBufferList();
        if (out.mNumberBuffers >= 2)
        {
            std::fill_n(static_cast<float*>(out.mBuffers[0].mData), frames, static_cast<float>(tempo / 1000.0));
            std::fill_n(static_cast<float*>(out.mBuffers[1].mData), frames, static_cast<float>(beat));
        }
        return result;
    }

private:
    Float64 expected = -1;
};

// Takes 48 and 96 kHz only, writing its rate / 100,000 to every channel.
class Rate final : public Checked
{
public:
    explicit Rate(AudioComponentInstance instance) : Checked(instance, "Rate") {}

    bool ValidFormat(AudioUnitScope scope, AudioUnitElement element, const AudioStreamBasicDescription& format) override
    {
        return (format.mSampleRate == 48000.0 || format.mSampleRate == 96000.0) &&
               Checked::ValidFormat(scope, element, format);
    }

protected:
    OSStatus process(AudioUnitRenderActionFlags&, const AudioBufferList&, AudioBufferList& out, UInt32 frames) override
    {
        const auto rate = static_cast<float>(Output(0).GetStreamFormat().mSampleRate / 100000.0);
        for (UInt32 c = 0; c < out.mNumberBuffers; ++c)
            std::fill_n(static_cast<float*>(out.mBuffers[c].mData), frames, rate);
        return noErr;
    }
};

// Fails every render shorter than its largest block, as units that need every block the same size do; its gain takes
// each change at its frame.
class FixedBlocks final : public Scheduled
{
public:
    explicit FixedBlocks(AudioComponentInstance instance) : Scheduled(instance, "FixedBlocks") {}

protected:
    OSStatus render(AudioUnitRenderActionFlags& flags, const AudioTimeStamp& time, UInt32 frames) override
    {
        if (frames != GetMaxFramesPerSlice())
            return kAudio_ParamError;
        return Checked::render(flags, time, frames);
    }
};

// Its latency goes from 10 to 20 ms at its fiftieth render, which it says then, and is never to be asked for while it
// renders.
class Late final : public Checked
{
public:
    explicit Late(AudioComponentInstance instance) : Checked(instance, "Late") {}

    Float64 GetLatency() override
    {
        if (rendering)
            misuse(name, "asked for its latency while rendering");
        return renders < 50 ? 0.010 : 0.020;
    }

protected:
    OSStatus render(AudioUnitRenderActionFlags& flags, const AudioTimeStamp& time, UInt32 frames) override
    {
        if (++renders == 50)
            PropertyChanged(kAudioUnitProperty_Latency, kAudioUnitScope_Global, 0);
        return Checked::render(flags, time, frames);
    }

private:
    std::atomic<int> renders{0};
};

// Parameters: a gain from 0 to 2 it takes at the frame of each change; a mode of three named steps; a meter of its
// output's level, which can't be written; an edit, which when set makes it move its gain itself in a gesture, as its
// own window would; and more, which when set gives it a sixth parameter and says its list changed. It keeps a document
// form of its state apart from its plain one.
class Gain final : public Scheduled
{
public:
    enum : AudioUnitParameterID
    {
        gain,
        mode,
        meter,
        edit,
        more,
        extra,
    };

    explicit Gain(AudioComponentInstance instance) : Scheduled(instance, "Gain")
    {
        Globals()->UseIndexedParameters(5);
        Globals()->SetParameter(gain, 1.0f);
        Globals()->SetParameter(mode, 1.0f);
    }

    OSStatus GetParameterInfo(AudioUnitScope scope, AudioUnitParameterID id, AudioUnitParameterInfo& info) override
    {
        if (scope != kAudioUnitScope_Global)
            return kAudioUnitErr_InvalidScope;
        info.flags = kAudioUnitParameterFlag_IsReadable | kAudioUnitParameterFlag_IsWritable;
        switch (id)
        {
            case gain:
                FillInParameterName(info, CFSTR("Gain"), false);
                info.unit = kAudioUnitParameterUnit_LinearGain;
                info.minValue = 0;
                info.maxValue = 2;
                info.defaultValue = 1;
                return noErr;
            case mode:
                FillInParameterName(info, CFSTR("Mode"), false);
                info.unit = kAudioUnitParameterUnit_Indexed;
                info.minValue = 0;
                info.maxValue = 2;
                info.defaultValue = 1;
                return noErr;
            case meter:
                FillInParameterName(info, CFSTR("Meter"), false);
                info.unit = kAudioUnitParameterUnit_Decibels;
                info.minValue = -60;
                info.maxValue = 0;
                info.flags = kAudioUnitParameterFlag_IsReadable;
                return noErr;
            case edit:
            case more:
            case extra:
                FillInParameterName(info,
                                    id == edit   ? CFSTR("Edit")
                                    : id == more ? CFSTR("More")
                                                 : CFSTR("Extra"),
                                    false);
                info.unit = kAudioUnitParameterUnit_Boolean;
                info.minValue = 0;
                info.maxValue = 1;
                return noErr;
            default: return kAudioUnitErr_InvalidParameter;
        }
    }

    OSStatus GetParameterValueStrings(AudioUnitScope scope, AudioUnitParameterID id, CFArrayRef* strings) override
    {
        if (scope != kAudioUnitScope_Global || id != mode)
            return kAudioUnitErr_InvalidProperty;
        if (strings != nullptr)
        {
            const void* names[] = {CFSTR("Soft"), CFSTR("Normal"), CFSTR("Hard")};
            *strings = CFArrayCreate(nullptr, names, 3, &kCFTypeArrayCallBacks);
        }
        return noErr;
    }

    // Its document form, its class info marked as a document's, which it takes as one and never as any other; its plain
    // form it won't take as a document's, as units that tell them apart may not.
    OSStatus GetPropertyInfo(AudioUnitPropertyID id, AudioUnitScope scope, AudioUnitElement element, UInt32& size,
                             bool& writable) override
    {
        if (id != kAudioUnitProperty_ClassInfoFromDocument || scope != kAudioUnitScope_Global)
            return Scheduled::GetPropertyInfo(id, scope, element, size, writable);
        size = sizeof(CFPropertyListRef);
        writable = true;
        return noErr;
    }

    OSStatus GetProperty(AudioUnitPropertyID id, AudioUnitScope scope, AudioUnitElement element, void* data) override
    {
        if (id != kAudioUnitProperty_ClassInfoFromDocument || scope != kAudioUnitScope_Global)
            return Scheduled::GetProperty(id, scope, element, data);
        CFPropertyListRef info = nullptr;
        if (const auto status = SaveState(&info); status != noErr)
            return status;
        const auto marked = CFDictionaryCreateMutableCopy(nullptr, 0, static_cast<CFDictionaryRef>(info));
        CFRelease(info);
        CFDictionarySetValue(marked, documentKey(), kCFBooleanTrue);
        *static_cast<CFPropertyListRef*>(data) = marked;
        return noErr;
    }

    OSStatus SetProperty(AudioUnitPropertyID id, AudioUnitScope scope, AudioUnitElement element, const void* data,
                         UInt32 size) override
    {
        if (id != kAudioUnitProperty_ClassInfoFromDocument || scope != kAudioUnitScope_Global)
            return Scheduled::SetProperty(id, scope, element, data, size);
        const auto info = *static_cast<const CFPropertyListRef*>(data);
        if (!isDocument(info))
            return kAudioUnitErr_InvalidPropertyValue;
        takingDocument = true;
        const auto result = RestoreState(info);
        takingDocument = false;
        return result;
    }

    OSStatus RestoreState(CFPropertyListRef info) override
    {
        if (!takingDocument && isDocument(info))
            misuse(name, "its document's state given as any other");
        return Scheduled::RestoreState(info);
    }

    OSStatus SetParameter(AudioUnitParameterID id, AudioUnitScope scope, AudioUnitElement element,
                          AudioUnitParameterValue value, UInt32 offset) override
    {
        if (id == meter)
            misuse(name, "its meter, which can't be written, was set");
        const auto result = Scheduled::SetParameter(id, scope, element, value, offset);
        if (id == edit && value >= 0.5f)
            editItself();
        if (id == more && value >= 0.5f && !grown)
        {
            grown = true;
            Globals()->UseIndexedParameters(6);
            PropertyChanged(kAudioUnitProperty_ParameterList, kAudioUnitScope_Global, 0);
        }
        return result;
    }

protected:
    // Its meter, the level of its last output sample in dB from -60 to 0.
    OSStatus process(AudioUnitRenderActionFlags& flags, const AudioBufferList& in, AudioBufferList& out,
                     UInt32 frames) override
    {
        const auto result = Scheduled::process(flags, in, out, frames);
        if (out.mNumberBuffers > 0 && frames > 0)
        {
            const auto last = std::abs(static_cast<const float*>(out.mBuffers[0].mData)[frames - 1]);
            Globals()->SetParameter(meter, std::clamp(last > 0 ? 20.0f * std::log10(last) : -60.0f, -60.0f, 0.0f));
        }
        return result;
    }

private:
    bool grown = false;
    bool takingDocument = false;

    static CFStringRef documentKey() { return CFSTR("SymbolEngine document"); }

    static bool isDocument(CFPropertyListRef info)
    {
        return info != nullptr && CFGetTypeID(info) == CFDictionaryGetTypeID() &&
               CFDictionaryContainsKey(static_cast<CFDictionaryRef>(info), documentKey());
    }

    // Its gain moved to 1.5 in a gesture of its own, as its window would, which listeners hear.
    void editItself()
    {
        AudioUnitEvent event{};
        event.mArgument.mParameter = {GetComponentInstance(), gain, kAudioUnitScope_Global, 0};
        event.mEventType = kAudioUnitEvent_BeginParameterChangeGesture;
        AUEventListenerNotify(nullptr, nullptr, &event);
        AUParameterSet(nullptr, nullptr, &event.mArgument.mParameter, 1.5f, 0);
        event.mEventType = kAudioUnitEvent_EndParameterChangeGesture;
        AUEventListenerNotify(nullptr, nullptr, &event);
    }
};

// Takes a state only once initialized, ignoring one given before, as some units do.
class Lazy final : public Scheduled
{
public:
    explicit Lazy(AudioComponentInstance instance) : Scheduled(instance, "Lazy") {}

    OSStatus RestoreState(CFPropertyListRef info) override
    {
        return IsInitialized() ? Scheduled::RestoreState(info) : noErr;
    }
};

// Takes a state only once the host has set its largest block and before it is initialized, ignoring it otherwise.
class Picky final : public Scheduled
{
public:
    explicit Picky(AudioComponentInstance instance) : Scheduled(instance, "Picky") {}

    OSStatus RestoreState(CFPropertyListRef info) override
    {
        return largestBlockSet() && !IsInitialized() ? Scheduled::RestoreState(info) : noErr;
    }
};

// Its gain goes back to 1 whenever it takes a state, as a unit whose state leaves a parameter out.
class Forget final : public Scheduled
{
public:
    explicit Forget(AudioComponentInstance instance) : Scheduled(instance, "Forget") {}

    OSStatus RestoreState(CFPropertyListRef info) override
    {
        const auto result = Scheduled::RestoreState(info);
        Globals()->SetParameter(0, 1.0f);
        return result;
    }
};

}

AUSDK_COMPONENT_ENTRY(ausdk::AUBaseFactory, Pass)
AUSDK_COMPONENT_ENTRY(ausdk::AUBaseFactory, Fail)
AUSDK_COMPONENT_ENTRY(ausdk::AUBaseFactory, Dead)
AUSDK_COMPONENT_ENTRY(ausdk::AUBaseFactory, Small)
AUSDK_COMPONENT_ENTRY(ausdk::AUBaseFactory, Pull)
AUSDK_COMPONENT_ENTRY(ausdk::AUBaseFactory, Swap)
AUSDK_COMPONENT_ENTRY(ausdk::AUBaseFactory, Echo)
AUSDK_COMPONENT_ENTRY(ausdk::AUBaseFactory, Ten)
AUSDK_COMPONENT_ENTRY(ausdk::AUBaseFactory, Grow)
AUSDK_COMPONENT_ENTRY(ausdk::AUBaseFactory, Time)
AUSDK_COMPONENT_ENTRY(ausdk::AUBaseFactory, Rate)
AUSDK_COMPONENT_ENTRY(ausdk::AUBaseFactory, FixedBlocks)
AUSDK_COMPONENT_ENTRY(ausdk::AUBaseFactory, Late)
AUSDK_COMPONENT_ENTRY(ausdk::AUBaseFactory, Gain)
AUSDK_COMPONENT_ENTRY(ausdk::AUBaseFactory, Lazy)
AUSDK_COMPONENT_ENTRY(ausdk::AUBaseFactory, Picky)
AUSDK_COMPONENT_ENTRY(ausdk::AUBaseFactory, Forget)
