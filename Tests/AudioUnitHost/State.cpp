// symbolengine_audio_unit_state <Speakers Test.component>
//
// Runs the Speakers Test plugin as an Audio Unit in this process and gives it saved states as hosts keep them: one with
// the plugin's state under the key its build names, speakersTestState, as a plugin released before kept its state, and
// one with the state only where the Audio Unit standard puts it. It checks that the plugin reads both, and saves its
// state under its own key beside the keys every Audio Unit's state has: before the unit is initialized, as a host
// opening a session restores it, while it is initialized, and after it is uninitialized, as a host may save it.

#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>

#include "RegisterUnit.h"

#include <cstdio>
#include <string>

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

    // Initialized, a state with nothing under the plugin's key is read from where the Audio Unit standard keeps it.
    check(AudioUnitInitialize(unit) == noErr, "The unit initialized");
    const auto standard = standardState(description);
    check(restoreAndRead(unit, standard, "Initialized") == -3.0f,
          "Initialized: the plugin read its state from the standard key");
    CFRelease(standard);
    savesAs("-3", "Initialized");

    // Uninitialized again, as a host may save it.
    AudioUnitUninitialize(unit);
    savesAs("-3", "Uninitialized");
    AudioComponentInstanceDispose(unit);
    std::printf("%d checks, %d failed\n", checks, failures);
    return failures == 0 && checks > 0 ? 0 : 1;
}
