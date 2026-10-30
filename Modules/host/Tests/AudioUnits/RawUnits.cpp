// Test Audio Units written straight against AudioToolbox, without Apple's AudioUnitSDK, for what that SDK never lets a
// unit do, as it copies a swapped output back and clears an output flagged silent: stereo effects that pass their input
// through, each misbehaving in how it gives its output back, as units not built on it can.

#include <AudioToolbox/AudioToolbox.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace
{

enum class Way
{
    ownBuffers,   // Its output in buffers of its own, the host's pointers replaced and the host's buffers untouched.
    saysSilent,   // Its sound where it belongs, while it flags its output as silent.
    shortCount,   // The whole block written, while the buffers' sizes say half of it was.
    noRight,      // Both channels written, while it says it gave back only one buffer.
    nullRight,    // A null pointer for the other channel.
};

struct Unit
{
    AudioComponentPlugInInterface interface;
    Way way = Way::ownBuffers;
    AudioStreamBasicDescription format{};
    UInt32 maximumFrames = 4096;
    AURenderCallbackStruct input{};
    bool initialized = false;
    std::vector<float> left, right;
};

AudioStreamBasicDescription stereo(Float64 rate)
{
    AudioStreamBasicDescription format{};
    format.mSampleRate = rate;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = static_cast<UInt32>(kAudioFormatFlagsNativeFloatPacked) | kAudioFormatFlagIsNonInterleaved;
    format.mBytesPerPacket = 4;
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = 4;
    format.mChannelsPerFrame = 2;
    format.mBitsPerChannel = 32;
    return format;
}

OSStatus initialize(void* self)
{
    auto* unit = static_cast<Unit*>(self);
    unit->left.assign(unit->maximumFrames, 0.0f);
    unit->right.assign(unit->maximumFrames, 0.0f);
    unit->initialized = true;
    return noErr;
}

OSStatus uninitialize(void* self)
{
    static_cast<Unit*>(self)->initialized = false;
    return noErr;
}

OSStatus getPropertyInfo(void*, AudioUnitPropertyID id, AudioUnitScope, AudioUnitElement, UInt32* size,
                         Boolean* writable)
{
    UInt32 needed = 0;
    Boolean canWrite = false;
    switch (id)
    {
        case kAudioUnitProperty_StreamFormat:
            needed = sizeof(AudioStreamBasicDescription);
            canWrite = true;
            break;
        case kAudioUnitProperty_MaximumFramesPerSlice:
            needed = sizeof(UInt32);
            canWrite = true;
            break;
        case kAudioUnitProperty_ElementCount: needed = sizeof(UInt32); break;
        case kAudioUnitProperty_Latency:
        case kAudioUnitProperty_TailTime: needed = sizeof(Float64); break;
        case kAudioUnitProperty_ParameterList: needed = 0; break;
        case kAudioUnitProperty_SetRenderCallback:
            needed = sizeof(AURenderCallbackStruct);
            canWrite = true;
            break;
        default: return kAudioUnitErr_InvalidProperty;
    }
    if (size != nullptr)
        *size = needed;
    if (writable != nullptr)
        *writable = canWrite;
    return noErr;
}

OSStatus getProperty(void* self, AudioUnitPropertyID id, AudioUnitScope scope, AudioUnitElement, void* data,
                     UInt32* size)
{
    auto* unit = static_cast<Unit*>(self);
    switch (id)
    {
        case kAudioUnitProperty_StreamFormat:
            *static_cast<AudioStreamBasicDescription*>(data) = unit->format;
            *size = sizeof(AudioStreamBasicDescription);
            return noErr;
        case kAudioUnitProperty_MaximumFramesPerSlice:
            *static_cast<UInt32*>(data) = unit->maximumFrames;
            *size = sizeof(UInt32);
            return noErr;
        case kAudioUnitProperty_ElementCount:
            *static_cast<UInt32*>(data) = scope == kAudioUnitScope_Input || scope == kAudioUnitScope_Output ? 1 : 0;
            *size = sizeof(UInt32);
            return noErr;
        case kAudioUnitProperty_Latency:
        case kAudioUnitProperty_TailTime:
            *static_cast<Float64*>(data) = 0.0;
            *size = sizeof(Float64);
            return noErr;
        case kAudioUnitProperty_ParameterList: *size = 0; return noErr;
        default: return kAudioUnitErr_InvalidProperty;
    }
}

OSStatus setProperty(void* self, AudioUnitPropertyID id, AudioUnitScope, AudioUnitElement, const void* data, UInt32)
{
    auto* unit = static_cast<Unit*>(self);
    switch (id)
    {
        case kAudioUnitProperty_StreamFormat:
            unit->format = *static_cast<const AudioStreamBasicDescription*>(data);
            return noErr;
        case kAudioUnitProperty_MaximumFramesPerSlice:
            unit->maximumFrames = *static_cast<const UInt32*>(data);
            return noErr;
        case kAudioUnitProperty_SetRenderCallback:
            unit->input = *static_cast<const AURenderCallbackStruct*>(data);
            return noErr;
        case kAudioUnitProperty_HostCallbacks:
        case kAudioUnitProperty_OfflineRender:
        case kAudioUnitProperty_SampleRate: return noErr;
        default: return kAudioUnitErr_InvalidProperty;
    }
}

OSStatus listener(void*, AudioUnitPropertyID, AudioUnitPropertyListenerProc, void*)
{
    return noErr;
}

OSStatus removeListener(void*, AudioUnitPropertyID, AudioUnitPropertyListenerProc, void*)
{
    return noErr;
}

OSStatus notify(void*, AURenderCallback, void*)
{
    return noErr;
}

OSStatus getParameter(void*, AudioUnitParameterID, AudioUnitScope, AudioUnitElement, AudioUnitParameterValue* value)
{
    *value = 0.0f;
    return kAudioUnitErr_InvalidParameter;
}

OSStatus setParameter(void*, AudioUnitParameterID, AudioUnitScope, AudioUnitElement, AudioUnitParameterValue, UInt32)
{
    return kAudioUnitErr_InvalidParameter;
}

OSStatus scheduleParameters(void*, const AudioUnitParameterEvent*, UInt32)
{
    return noErr;
}

OSStatus reset(void*, AudioUnitScope, AudioUnitElement)
{
    return noErr;
}

OSStatus render(void* self, AudioUnitRenderActionFlags* flags, const AudioTimeStamp* time, UInt32, UInt32 frames,
                AudioBufferList* output)
{
    auto* unit = static_cast<Unit*>(self);
    if (!unit->initialized)
        return kAudioUnitErr_Uninitialized;
    if (frames > unit->maximumFrames)
        return kAudioUnitErr_TooManyFramesToProcess;

    // Its input, pulled into its own buffers.
    std::vector<std::byte> room(sizeof(AudioBufferList) + sizeof(AudioBuffer));
    auto* input = reinterpret_cast<AudioBufferList*>(room.data());
    input->mNumberBuffers = 2;
    input->mBuffers[0] = {1, frames * 4, unit->left.data()};
    input->mBuffers[1] = {1, frames * 4, unit->right.data()};
    AudioUnitRenderActionFlags pulled = 0;
    if (const auto status = unit->input.inputProc(unit->input.inputProcRefCon, &pulled, time, 0, frames, input);
        status != noErr)
        return status;

    const auto write = [&](UInt32 channel, UInt32 count) {
        std::copy_n(channel == 0 ? unit->left.data() : unit->right.data(), count,
                    static_cast<float*>(output->mBuffers[channel].mData));
    };
    switch (unit->way)
    {
        case Way::ownBuffers:
            output->mBuffers[0].mData = unit->left.data();
            output->mBuffers[1].mData = unit->right.data();
            break;
        case Way::saysSilent:
            write(0, frames);
            write(1, frames);
            *flags |= kAudioUnitRenderAction_OutputIsSilence;
            break;
        case Way::shortCount:
            write(0, frames);
            write(1, frames);
            output->mBuffers[0].mDataByteSize = (frames / 2) * 4;
            output->mBuffers[1].mDataByteSize = (frames / 2) * 4;
            break;
        case Way::noRight:
            write(0, frames);
            write(1, frames);
            output->mNumberBuffers = 1;
            break;
        case Way::nullRight:
            write(0, frames);
            output->mBuffers[1].mData = nullptr;
            break;
    }
    return noErr;
}

OSStatus open(void* self, AudioComponentInstance)
{
    static_cast<Unit*>(self)->format = stereo(48000.0);
    return noErr;
}

OSStatus close(void* self)
{
    delete static_cast<Unit*>(self);
    return noErr;
}

AudioComponentMethod lookup(SInt16 selector)
{
    switch (selector)
    {
        case kAudioUnitInitializeSelect: return reinterpret_cast<AudioComponentMethod>(initialize);
        case kAudioUnitUninitializeSelect: return reinterpret_cast<AudioComponentMethod>(uninitialize);
        case kAudioUnitGetPropertyInfoSelect: return reinterpret_cast<AudioComponentMethod>(getPropertyInfo);
        case kAudioUnitGetPropertySelect: return reinterpret_cast<AudioComponentMethod>(getProperty);
        case kAudioUnitSetPropertySelect: return reinterpret_cast<AudioComponentMethod>(setProperty);
        case kAudioUnitAddPropertyListenerSelect: return reinterpret_cast<AudioComponentMethod>(listener);
        case kAudioUnitRemovePropertyListenerWithUserDataSelect:
            return reinterpret_cast<AudioComponentMethod>(removeListener);
        case kAudioUnitAddRenderNotifySelect:
        case kAudioUnitRemoveRenderNotifySelect: return reinterpret_cast<AudioComponentMethod>(notify);
        case kAudioUnitGetParameterSelect: return reinterpret_cast<AudioComponentMethod>(getParameter);
        case kAudioUnitSetParameterSelect: return reinterpret_cast<AudioComponentMethod>(setParameter);
        case kAudioUnitScheduleParametersSelect: return reinterpret_cast<AudioComponentMethod>(scheduleParameters);
        case kAudioUnitResetSelect: return reinterpret_cast<AudioComponentMethod>(reset);
        case kAudioUnitRenderSelect: return reinterpret_cast<AudioComponentMethod>(render);
        default: return nullptr;
    }
}

void* make(Way way)
{
    auto* unit = new Unit;
    unit->interface = {open, close, lookup, nullptr};
    unit->way = way;
    return unit;
}

}

extern "C"
{
    __attribute__((visibility("default"))) void* OwnBuffersFactory(const AudioComponentDescription*)
    {
        return make(Way::ownBuffers);
    }
    __attribute__((visibility("default"))) void* SaysSilentFactory(const AudioComponentDescription*)
    {
        return make(Way::saysSilent);
    }
    __attribute__((visibility("default"))) void* ShortCountFactory(const AudioComponentDescription*)
    {
        return make(Way::shortCount);
    }
    __attribute__((visibility("default"))) void* NoRightFactory(const AudioComponentDescription*)
    {
        return make(Way::noRight);
    }
    __attribute__((visibility("default"))) void* NullRightFactory(const AudioComponentDescription*)
    {
        return make(Way::nullRight);
    }
}
