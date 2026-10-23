// symbolengine_audio_unit_layouts <Speakers Test.component>
//
// Runs the Speakers Test plugin as an Audio Unit in this process, as a host does, in each layout a host sets, and in
// channel counts alone. Each channel goes in as a value of its own, 100 for the first, 200 for the second and so on,
// and the plugin adds its speaker's number plus one, so each channel coming out says whether it reached the plugin as
// the speaker the layout names and came back where it went in. The speakers each layout names are written out below
// from Core Audio's own list, in the layout's order, as CLAP numbers them.

#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
enum : int
{
    L = 0,
    R = 1,
    C = 2,
    LFE = 3,
    BL = 4,
    BR = 5,
    SL = 9,
    SR = 10,
    TFL = 12,
    TFR = 14,
    TBL = 15,
    TBR = 17,
    TSL = 18,
    TSR = 19
};

struct Case
{
    const char* name;
    AudioChannelLayoutTag tag;          // 0 for a channel count alone
    std::vector<int> speakers;          // in the AU's channel order
    AudioChannelLayoutTag chosen = 0;   // the layout the unit should say it chose, for a channel count alone
};

const std::vector<Case> cases{
    {"Mono", kAudioChannelLayoutTag_Mono, {C}},
    {"Stereo", kAudioChannelLayoutTag_Stereo, {L, R}},
    {"5.1, L R C LFE Ls Rs", kAudioChannelLayoutTag_MPEG_5_1_A, {L, R, C, LFE, BL, BR}},
    {"5.1, L C R Ls Rs LFE", kAudioChannelLayoutTag_MPEG_5_1_C, {L, C, R, BL, BR, LFE}},
    {"7.1, L R C LFE Ls Rs Rls Rrs", kAudioChannelLayoutTag_MPEG_7_1_C, {L, R, C, LFE, SL, SR, BL, BR}},
    {"Logic's 7.1, L R Ls Rs Rls Rrs C LFE", kAudioChannelLayoutTag_Logic_7_1_B, {L, R, SL, SR, BL, BR, C, LFE}},
    {"Atmos 5.1.2", kAudioChannelLayoutTag_Atmos_5_1_2, {L, R, C, LFE, BL, BR, TSL, TSR}},
    {"Atmos 7.1.4", kAudioChannelLayoutTag_Atmos_7_1_4, {L, R, C, LFE, SL, SR, BL, BR, TFL, TFR, TBL, TBR}},
    {"Logic's Atmos 7.1.4, L R Rls Rrs Ls Rs C LFE ...",
     kAudioChannelLayoutTag_Logic_Atmos_7_1_4_B,
     {L, R, BL, BR, SL, SR, C, LFE, TFL, TFR, TBL, TBR}},
    {"6 channels alone", 0, {L, R, C, LFE, BL, BR}, kAudioChannelLayoutTag_MPEG_5_1_A},
    {"8 channels alone", 0, {L, R, C, LFE, SL, SR, BL, BR}, kAudioChannelLayoutTag_MPEG_7_1_C},
    {"12 channels alone", 0, {L, R, C, LFE, SL, SR, BL, BR, TFL, TFR, TBL, TBR}, kAudioChannelLayoutTag_Atmos_7_1_4},
};

constexpr UInt32 frames = 64;
int failures = 0;
int checks = 0;

void check(bool ok, const std::string& what)
{
    ++checks;
    if (!ok)
    {
        ++failures;
        std::printf("FAILED: %s\n", what.c_str());
    }
}

OSType fourCharacters(CFDictionaryRef dictionary, CFStringRef key)
{
    char text[8] = {};
    const auto value = static_cast<CFStringRef>(CFDictionaryGetValue(dictionary, key));
    if (value == nullptr || !CFStringGetCString(value, text, sizeof text, kCFStringEncodingASCII))
        return 0;
    return static_cast<OSType>(text[0]) << 24 | static_cast<OSType>(text[1]) << 16 | static_cast<OSType>(text[2]) << 8 |
           static_cast<OSType>(text[3]);
}

// Registers the bundle's Audio Unit with this process only, as a host's own copy, so nothing is installed.
AudioComponent registerUnit(const char* path)
{
    const auto url =
        CFURLCreateFromFileSystemRepresentation(nullptr, reinterpret_cast<const UInt8*>(path),
                                                static_cast<CFIndex>(std::char_traits<char>::length(path)), true);
    const auto bundle = CFBundleCreate(nullptr, url);
    CFRelease(url);
    if (bundle == nullptr || !CFBundleLoadExecutable(bundle))
        return nullptr;

    const auto info = CFBundleGetInfoDictionary(bundle);
    const auto list = static_cast<CFArrayRef>(CFDictionaryGetValue(info, CFSTR("AudioComponents")));
    if (list == nullptr || CFArrayGetCount(list) < 1)
        return nullptr;
    const auto entry = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(list, 0));

    AudioComponentDescription description{};
    description.componentType = fourCharacters(entry, CFSTR("type"));
    description.componentSubType = fourCharacters(entry, CFSTR("subtype"));
    description.componentManufacturer = fourCharacters(entry, CFSTR("manufacturer"));

    const auto factoryName = static_cast<CFStringRef>(CFDictionaryGetValue(entry, CFSTR("factoryFunction")));
    const auto factory =
        reinterpret_cast<AudioComponentFactoryFunction>(CFBundleGetFunctionPointerForName(bundle, factoryName));
    if (factory == nullptr)
        return nullptr;
    return AudioComponentRegister(&description, CFSTR("SymbolEngine: Speakers Test, in process"), 1, factory);
}

OSStatus giveInput(void*, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32, UInt32 count,
                   AudioBufferList* buffers)
{
    for (UInt32 channel = 0; channel < buffers->mNumberBuffers; ++channel)
    {
        auto* samples = static_cast<float*>(buffers->mBuffers[channel].mData);
        if (samples != nullptr)
            std::fill(samples, samples + count, 100.0f * static_cast<float>(channel + 1));
    }
    return noErr;
}

std::vector<AudioChannelLayoutTag> layoutTags(AudioUnit unit, AudioUnitScope scope)
{
    UInt32 size = 0;
    if (AudioUnitGetPropertyInfo(unit, kAudioUnitProperty_SupportedChannelLayoutTags, scope, 0, &size, nullptr) !=
        noErr)
        return {};
    std::vector<AudioChannelLayoutTag> tags(size / sizeof(AudioChannelLayoutTag));
    AudioUnitGetProperty(unit, kAudioUnitProperty_SupportedChannelLayoutTags, scope, 0, tags.data(), &size);
    return tags;
}

void run(AudioComponent component, const Case& test)
{
    AudioUnit unit = nullptr;
    if (AudioComponentInstanceNew(component, &unit) != noErr || unit == nullptr)
    {
        check(false, std::string(test.name) + ": the unit could not be made");
        return;
    }

    const auto channels = static_cast<UInt32>(test.speakers.size());
    AudioStreamBasicDescription format{};
    format.mSampleRate = 48000.0;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags =
        static_cast<AudioFormatFlags>(kAudioFormatFlagsNativeFloatPacked) | kAudioFormatFlagIsNonInterleaved;
    format.mBytesPerPacket = sizeof(float);
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = sizeof(float);
    format.mChannelsPerFrame = channels;
    format.mBitsPerChannel = 32;

    bool ready = true;
    for (const auto scope : {kAudioUnitScope_Input, kAudioUnitScope_Output})
    {
        ready &= AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, scope, 0, &format, sizeof format) == noErr;
        if (test.tag != 0)
        {
            AudioChannelLayout layout{};
            layout.mChannelLayoutTag = test.tag;
            ready &= AudioUnitSetProperty(unit, kAudioUnitProperty_AudioChannelLayout, scope, 0, &layout,
                                          sizeof layout) == noErr;
        }
    }
    AURenderCallbackStruct input{giveInput, nullptr};
    ready &= AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &input,
                                  sizeof input) == noErr;
    ready &= AudioUnitInitialize(unit) == noErr;
    check(ready, std::string(test.name) + ": the unit took the format and layout, and initialized");

    if (ready)
    {
        std::vector<std::vector<float>> samples(channels, std::vector<float>(frames, -1.0f));
        std::vector<std::byte> storage(offsetof(AudioBufferList, mBuffers) + channels * sizeof(AudioBuffer));
        auto* output = reinterpret_cast<AudioBufferList*>(storage.data());
        output->mNumberBuffers = channels;
        for (UInt32 channel = 0; channel < channels; ++channel)
            output->mBuffers[channel] = {1, frames * static_cast<UInt32>(sizeof(float)), samples[channel].data()};

        AudioTimeStamp time{};
        time.mSampleTime = 0;
        time.mFlags = kAudioTimeStampSampleTimeValid;
        AudioUnitRenderActionFlags flags = 0;
        const auto rendered = AudioUnitRender(unit, &flags, &time, 0, frames, output);
        check(rendered == noErr, std::string(test.name) + ": rendered");

        for (UInt32 channel = 0; channel < channels && rendered == noErr; ++channel)
        {
            const auto* got = static_cast<const float*>(output->mBuffers[channel].mData);
            const float expected =
                100.0f * static_cast<float>(channel + 1) + static_cast<float>(test.speakers[channel] + 1);
            const bool all = std::all_of(got, got + frames, [expected](float value) { return value == expected; });
            check(all, std::string(test.name) + ": channel " + std::to_string(channel + 1) + " should be " +
                           std::to_string(expected) + " but is " + std::to_string(got[0]));
        }

        if (test.chosen != 0)
        {
            AudioChannelLayout layout{};
            UInt32 size = sizeof layout;
            const auto read = AudioUnitGetProperty(unit, kAudioUnitProperty_AudioChannelLayout, kAudioUnitScope_Output,
                                                   0, &layout, &size);
            check(read == noErr && layout.mChannelLayoutTag == test.chosen,
                  std::string(test.name) + ": the unit says it chose the expected layout");
        }
        AudioUnitUninitialize(unit);
    }
    AudioComponentInstanceDispose(unit);
}
}

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::printf("usage: symbolengine_audio_unit_layouts <Speakers Test.component>\n");
        return 2;
    }

    const auto component = registerUnit(argv[1]);
    if (component == nullptr)
    {
        std::printf("FAILED: %s has no Audio Unit this test could register\n", argv[1]);
        return 1;
    }

    // What the unit offers: the standard layouts it takes, and not the 7.1 with left and right center it doesn't.
    AudioUnit unit = nullptr;
    if (AudioComponentInstanceNew(component, &unit) == noErr)
    {
        const auto tags = layoutTags(unit, kAudioUnitScope_Output);
        const auto has = [&tags](AudioChannelLayoutTag tag) {
            return std::find(tags.begin(), tags.end(), tag) != tags.end();
        };
        std::printf("The unit offers %zu output layouts\n", tags.size());
        check(has(kAudioChannelLayoutTag_Atmos_7_1_4), "Atmos 7.1.4 is offered");
        check(has(kAudioChannelLayoutTag_MPEG_5_1_A), "5.1 is offered");
        check(!has(kAudioChannelLayoutTag_MPEG_7_1_A), "7.1 with left and right center isn't offered");
        check(layoutTags(unit, kAudioUnitScope_Input) == tags, "the input offers the same layouts");
        AudioComponentInstanceDispose(unit);
    }
    else
        check(false, "the unit could be made");

    for (const auto& test : cases)
        run(component, test);

    std::printf("%d checks over %zu layouts, %d failed\n", checks, cases.size(), failures);
    return failures == 0 && checks > 0 ? 0 : 1;
}
