#pragma once

// Registers a bundle's Audio Unit with this process only, as a host's own copy, so a test runs it without installing it.

#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>

#include <string>

inline OSType fourCharacters(CFDictionaryRef dictionary, CFStringRef key)
{
    char text[8] = {};
    const auto value = static_cast<CFStringRef>(CFDictionaryGetValue(dictionary, key));
    if (value == nullptr || !CFStringGetCString(value, text, sizeof text, kCFStringEncodingASCII))
        return 0;
    return static_cast<OSType>(text[0]) << 24 | static_cast<OSType>(text[1]) << 16 | static_cast<OSType>(text[2]) << 8 |
           static_cast<OSType>(text[3]);
}

inline AudioComponent registerUnit(const char* path)
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
    return AudioComponentRegister(&description, CFSTR("SymbolEngine test unit, in process"), 1, factory);
}
