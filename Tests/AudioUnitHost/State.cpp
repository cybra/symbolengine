// symbolengine_audio_unit_state <Speakers Test.component>
//
// Runs the Speakers Test plugin as an Audio Unit in this process and gives it saved states as hosts keep them: one with
// the plugin's state under the key its build names, speakersTestState, as a plugin released before kept its state, and
// one with the state only where the Audio Unit standard puts it. It checks that the plugin reads both, and saves its
// state under its own key beside the keys every Audio Unit's state has: before the unit is initialized, as a host
// opening a session restores it, while it is initialized, and after it is uninitialized, as a host may save it. A value
// the host sets is in the state it saves straight after, with no render or idle tick between to hand it to the plugin:
// before initializing, initialized with nothing rendered, just after a render, as when a host stops, and while another
// thread renders, as when a host plays, with no block coming out silent.

#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>

#include "RegisterUnit.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace
{
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

void setNumber(CFMutableDictionaryRef dictionary, CFStringRef key, SInt32 value)
{
    const auto number = CFNumberCreate(nullptr, kCFNumberSInt32Type, &value);
    CFDictionarySetValue(dictionary, key, number);
    CFRelease(number);
}

void setData(CFMutableDictionaryRef dictionary, CFStringRef key, const std::string& bytes)
{
    const auto data =
        CFDataCreate(nullptr, reinterpret_cast<const UInt8*>(bytes.data()), static_cast<CFIndex>(bytes.size()));
    CFDictionarySetValue(dictionary, key, data);
    CFRelease(data);
}

// A saved state as a host keeps it: the unit's identity, and the plugin's state under the key given.
CFMutableDictionaryRef savedState(const AudioComponentDescription& unit, CFStringRef key, const std::string& state)
{
    const auto dictionary =
        CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    setNumber(dictionary, CFSTR("version"), 0);
    setNumber(dictionary, CFSTR("type"), static_cast<SInt32>(unit.componentType));
    setNumber(dictionary, CFSTR("subtype"), static_cast<SInt32>(unit.componentSubType));
    setNumber(dictionary, CFSTR("manufacturer"), static_cast<SInt32>(unit.componentManufacturer));
    CFDictionarySetValue(dictionary, CFSTR("name"), CFSTR("Saved"));
    setData(dictionary, CFSTR("data"), "");
    setData(dictionary, key, state);
    return dictionary;
}

std::string dataAt(CFDictionaryRef dictionary, CFStringRef key)
{
    const auto data = static_cast<CFDataRef>(CFDictionaryGetValue(dictionary, key));
    if (data == nullptr || CFGetTypeID(data) != CFDataGetTypeID())
        return {};
    return std::string(reinterpret_cast<const char*>(CFDataGetBytePtr(data)),
                       static_cast<std::size_t>(CFDataGetLength(data)));
}

// A state with nothing under the plugin's key, and its state where the Audio Unit standard keeps it.
CFMutableDictionaryRef standardState(const AudioComponentDescription& unit)
{
    const auto standard = savedState(unit, CFSTR("other"), "");
    setData(standard, CFSTR("data"), R"({"parameters":{"1":-3,"2":0}})");
    return standard;
}

OSStatus giveSilence(void*, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32, UInt32 count,
                     AudioBufferList* buffers)
{
    for (UInt32 channel = 0; channel < buffers->mNumberBuffers; ++channel)
        if (buffers->mBuffers[channel].mData != nullptr)
            std::fill_n(static_cast<float*>(buffers->mBuffers[channel].mData), count, 0.0f);
    return noErr;
}

float restoreAndRead(AudioUnit unit, CFDictionaryRef saved, const std::string& what)
{
    CFPropertyListRef list = saved;
    check(AudioUnitSetProperty(unit, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0, &list, sizeof list) ==
              noErr,
          what + ": the unit took the saved state");
    AudioUnitParameterValue value = 0;
    AudioUnitGetParameter(unit, 1, kAudioUnitScope_Global, 0, &value);
    return value;
}
}

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::printf("usage: symbolengine_audio_unit_state <Speakers Test.component>\n");
        return 2;
    }

    const auto component = registerUnit(argv[1]);
    AudioUnit unit = nullptr;
    if (component == nullptr || AudioComponentInstanceNew(component, &unit) != noErr)
    {
        std::printf("FAILED: %s has no Audio Unit this test could make\n", argv[1]);
        return 1;
    }
    AudioComponentDescription description{};
    AudioComponentGetDescription(component, &description);

    // What it saves: its state under its own key, beside the keys every Audio Unit's state has.
    const auto savesAs = [&](const char* value, const std::string& when) {
        CFPropertyListRef saved = nullptr;
        UInt32 size = sizeof saved;
        check(AudioUnitGetProperty(unit, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0, &saved, &size) ==
                      noErr &&
                  saved != nullptr,
              when + ": the unit saved its state");
        if (saved == nullptr)
            return;
        const auto dictionary = static_cast<CFDictionaryRef>(saved);
        const auto state = dataAt(dictionary, CFSTR("speakersTestState"));
        check(state.find(std::string(R"("1":)") + value) != std::string::npos,
              when + ": it saved the plugin's state under its own key: " + state);
        check(CFDictionaryContainsKey(dictionary, CFSTR("data")) && CFDictionaryContainsKey(dictionary, CFSTR("type")),
              when + ": it saved the keys every Audio Unit's state has");
        CFRelease(saved);
    };

    // Before it is initialized, as a host opening a session restores it: the plugin's state under its own key, as a
    // plugin released before kept it.
    const auto own = savedState(description, CFSTR("speakersTestState"), R"({"parameters":{"1":6.5,"2":0}})");
    check(restoreAndRead(unit, own, "Before initializing") == 6.5f, "Before initializing: the plugin read its state");
    CFRelease(own);
    savesAs("6.5", "Before initializing");
    AudioUnitSetParameter(unit, 1, kAudioUnitScope_Global, 0, 4.25f, 0);
    savesAs("4.25", "Set before initializing");

    // Initialized, a state with nothing under the plugin's key is read from where the Audio Unit standard keeps it.
    AURenderCallbackStruct input{giveSilence, nullptr};
    AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &input, sizeof input);
    check(AudioUnitInitialize(unit) == noErr, "The unit initialized");
    const auto standard = standardState(description);
    check(restoreAndRead(unit, standard, "Initialized") == -3.0f,
          "Initialized: the plugin read its state from the standard key");
    CFRelease(standard);
    savesAs("-3", "Initialized");
    AudioUnitSetParameter(unit, 1, kAudioUnitScope_Global, 0, 2.5f, 0);
    savesAs("2.5", "Set while initialized, with nothing rendered");

    // A block rendered, and then none: the state waits no longer than a few blocks for a render to hand the value over.
    AudioStreamBasicDescription format{};
    UInt32 size = sizeof format;
    AudioUnitGetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &format, &size);
    const auto channels = format.mChannelsPerFrame;
    constexpr UInt32 frames = 512;
    std::vector<std::vector<float>> samples(channels, std::vector<float>(frames));
    std::vector<std::byte> storage(offsetof(AudioBufferList, mBuffers) + channels * sizeof(AudioBuffer));
    auto* output = reinterpret_cast<AudioBufferList*>(storage.data());
    output->mNumberBuffers = channels;
    for (UInt32 channel = 0; channel < channels; ++channel)
        output->mBuffers[channel] = {1, frames * static_cast<UInt32>(sizeof(float)), samples[channel].data()};
    AudioTimeStamp time{};
    time.mFlags = kAudioTimeStampSampleTimeValid;
    AudioUnitRenderActionFlags flags = 0;
    check(channels > 0 && AudioUnitRender(unit, &flags, &time, 0, frames, output) == noErr, "The unit rendered a block");
    AudioUnitSetParameter(unit, 1, kAudioUnitScope_Global, 0, 1.75f, 0);
    const auto started = std::chrono::steady_clock::now();
    savesAs("1.75", "Set just after a render");
    const auto waited = std::chrono::steady_clock::now() - started;
    check(waited < std::chrono::seconds(1), "Set just after a render: the state came within a second");

    // While a host plays, rendering a block every 2 ms on its own thread: the state has the value, and no block came out
    // silent, as one would if saving held up a render. Each channel comes out marked with its speaker, never 0. A save
    // waits for the next render only while the host rendered within the last three blocks, so a block after the host
    // itself went more than two blocks' time without one, as a busy machine can make it, is counted apart. A failure says
    // which block came out silent and how long the host waited before it.
    std::atomic<bool> playing{true};
    std::atomic<int> blocks{0}, silent{0}, afterLate{0}, firstSilent{-1};
    std::atomic<double> gapBeforeSilent{0.0};
    const auto lateAfter = std::chrono::duration<double>(2.0 * frames / format.mSampleRate);
    std::thread host([&] {
        auto stamp = time;
        auto lastEnded = std::chrono::steady_clock::time_point{};
        while (playing)
        {
            const auto started = std::chrono::steady_clock::now();
            const auto gap = std::chrono::duration<double, std::milli>(started - lastEnded).count();
            const auto late = lastEnded != std::chrono::steady_clock::time_point{} && started - lastEnded > lateAfter;
            stamp.mSampleTime += frames;
            AudioUnitRenderActionFlags playFlags = 0;
            if (AudioUnitRender(unit, &playFlags, &stamp, 0, frames, output) == noErr)
            {
                ++blocks;
                bool quiet = false;
                for (UInt32 channel = 0; channel < channels; ++channel)
                {
                    const auto* got = static_cast<const float*>(output->mBuffers[channel].mData);
                    quiet |= std::any_of(got, got + frames, [](float value) { return value == 0.0f; });
                }
                (late ? afterLate : silent) += quiet ? 1 : 0;
                if (quiet && !late && firstSilent < 0)
                {
                    gapBeforeSilent = gap;
                    firstSilent = blocks - 1;
                }
            }
            lastEnded = std::chrono::steady_clock::now();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    AudioUnitSetParameter(unit, 1, kAudioUnitScope_Global, 0, 0.5f, 0);
    savesAs("0.5", "Set while playing");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    playing = false;
    host.join();
    check(blocks > 10 && silent == 0, "Set while playing: " + std::to_string(blocks.load()) + " blocks rendered, " +
                                          std::to_string(silent.load()) + " of them silent, the first block " +
                                          std::to_string(firstSilent.load()) + " after a gap of " +
                                          std::to_string(gapBeforeSilent.load()) + " ms, and " +
                                          std::to_string(afterLate.load()) + " silent after the host was late");

    // Uninitialized again, as a host may save it.
    AudioUnitUninitialize(unit);
    savesAs("0.5", "Uninitialized");
    AudioComponentInstanceDispose(unit);
    std::printf("%d checks, %d failed\n", checks, failures);
    return failures == 0 && checks > 0 ? 0 : 1;
}
