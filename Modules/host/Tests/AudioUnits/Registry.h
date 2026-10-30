// Registers the test component's units with this process only, as the tests run them uninstalled: each unit's codes,
// name and factory function, from its Info.plist. Used by the tests and by the scan worker program they start.

#pragma once

#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>

#include <filesystem>
#include <map>
#include <string>

namespace symbolengine::testAudioUnits
{

// A component's bundle, its code loaded, once for the process for each component; null where it can't be.
inline CFBundleRef bundle(const std::filesystem::path& component)
{
    static std::map<std::string, CFBundleRef> loaded;
    const auto path = component.string();
    if (const auto found = loaded.find(path); found != loaded.end())
        return found->second;
    const auto url = CFURLCreateFromFileSystemRepresentation(nullptr, reinterpret_cast<const UInt8*>(path.c_str()),
                                                             static_cast<CFIndex>(path.size()), true);
    auto made = CFBundleCreate(nullptr, url);
    CFRelease(url);
    if (made != nullptr && !CFBundleLoadExecutable(made))
    {
        CFRelease(made);
        made = nullptr;
    }
    loaded[path] = made;
    return made;
}

inline AudioComponentFactoryFunction factory(const std::filesystem::path& component, CFStringRef name)
{
    const auto loaded = bundle(component);
    return loaded != nullptr
               ? reinterpret_cast<AudioComponentFactoryFunction>(CFBundleGetFunctionPointerForName(loaded, name))
               : nullptr;
}

inline OSType codeOf(CFDictionaryRef entry, CFStringRef key)
{
    char text[8] = {};
    const auto value = CFDictionaryGetValue(entry, key);
    if (value == nullptr || CFGetTypeID(value) != CFStringGetTypeID() ||
        !CFStringGetCString(static_cast<CFStringRef>(value), text, sizeof text, kCFStringEncodingASCII))
        return 0;
    return static_cast<OSType>(static_cast<unsigned char>(text[0])) << 24 |
           static_cast<OSType>(static_cast<unsigned char>(text[1])) << 16 |
           static_cast<OSType>(static_cast<unsigned char>(text[2])) << 8 |
           static_cast<OSType>(static_cast<unsigned char>(text[3]));
}

// Every unit a component declares, registered once for the process; false if any can't be.
inline bool registerAll(const std::filesystem::path& component)
{
    static std::map<std::string, bool> done;
    if (const auto found = done.find(component.string()); found != done.end())
        return found->second;
    const bool registered = [&component] {
        const auto loaded = bundle(component);
        if (loaded == nullptr)
            return false;
        const auto list =
            static_cast<CFArrayRef>(CFDictionaryGetValue(CFBundleGetInfoDictionary(loaded), CFSTR("AudioComponents")));
        if (list == nullptr)
            return false;
        for (CFIndex i = 0; i < CFArrayGetCount(list); ++i)
        {
            const auto entry = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(list, i));
            const AudioComponentDescription description{codeOf(entry, CFSTR("type")), codeOf(entry, CFSTR("subtype")),
                                                        codeOf(entry, CFSTR("manufacturer")), 0, 0};
            const auto made =
                factory(component, static_cast<CFStringRef>(CFDictionaryGetValue(entry, CFSTR("factoryFunction"))));
            if (made == nullptr ||
                AudioComponentRegister(&description,
                                       static_cast<CFStringRef>(CFDictionaryGetValue(entry, CFSTR("name"))), 1,
                                       made) == nullptr)
                return false;
        }
        return true;
    }();
    done[component.string()] = registered;
    return registered;
}

}
