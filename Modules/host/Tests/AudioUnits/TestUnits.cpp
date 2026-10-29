// The host's test Audio Units, built on Apple's AudioUnitSDK, one component holding many. Each checks the calls a host
// makes into it: everything but rendering on the main thread, nothing overlapping a render, its largest block and every
// bus's format set before it is initialized; and writes what was wrong to the file SYMBOLENGINE_TEST_AU_LOG names. Most
// also misbehave one way, as units do.

#include <AudioUnitSDK/AUEffectBase.h>

#include <pthread.h>

#include <algorithm>
#include <atomic>
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

    OSStatus Render(AudioUnitRenderActionFlags& flags, const AudioTimeStamp& time, UInt32 frames) override
    {
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
