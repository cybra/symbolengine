#include "Vst3Module.h"

#include <CoreFoundation/CoreFoundation.h>

#include <string>

namespace symbolengine
{

struct Vst3Module::Native
{
    CFBundleRef bundle = nullptr;
    bool (*exit)() = nullptr;
};

namespace
{

std::string text(CFStringRef string)
{
    if (string == nullptr)
        return {};

    const auto length = CFStringGetMaximumSizeForEncoding(CFStringGetLength(string), kCFStringEncodingUTF8) + 1;
    std::string out(static_cast<std::size_t>(length), '\0');

    if (!CFStringGetCString(string, out.data(), length, kCFStringEncodingUTF8))
        return {};

    out.resize(std::char_traits<char>::length(out.c_str()));
    return out;
}

}

std::shared_ptr<Vst3Module::Native> Vst3Module::load(const std::filesystem::path& location,
                                                     Steinberg::IPluginFactory*& factory, PluginError& error)
{
    const auto& path = location.native();
    const CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, reinterpret_cast<const UInt8*>(path.c_str()),
                                                                 static_cast<CFIndex>(path.size()), true);
    const CFBundleRef bundle = url != nullptr ? CFBundleCreate(nullptr, url) : nullptr;

    if (url != nullptr)
        CFRelease(url);

    if (bundle == nullptr)
    {
        error = {"It isn't a bundle the system can open.", "load", 0};
        return nullptr;
    }

    CFErrorRef failure = nullptr;

    if (!CFBundleLoadExecutableAndReturnError(bundle, &failure))
    {
        std::string why = "Its code couldn't be loaded.";

        if (failure != nullptr)
        {
            const CFStringRef description = CFErrorCopyDescription(failure);
            why = text(description);
            CFRelease(description);
            CFRelease(failure);
        }

        error = {why, "load", 0};
        CFRelease(bundle);
        return nullptr;
    }

    // The bundle's entry comes first and is required on a Mac; its factory after.
    using Entry = bool (*)(CFBundleRef);
    using Factory = Steinberg::IPluginFactory* (*) ();
    const auto entry = reinterpret_cast<Entry>(CFBundleGetFunctionPointerForName(bundle, CFSTR("bundleEntry")));
    const auto getFactory =
        reinterpret_cast<Factory>(CFBundleGetFunctionPointerForName(bundle, CFSTR("GetPluginFactory")));
    auto native = std::make_shared<Native>();
    native->bundle = bundle;
    native->exit = reinterpret_cast<bool (*)()>(CFBundleGetFunctionPointerForName(bundle, CFSTR("bundleExit")));

    if (entry == nullptr || getFactory == nullptr || native->exit == nullptr)
    {
        error = {"It lacks a function every VST3 bundle has (bundleEntry, bundleExit or GetPluginFactory).", "load", 0};
        return nullptr;
    }

    if (!entry(bundle))
    {
        error = {"Its entry function refused to start it.", "bundleEntry", 0};
        native->exit = nullptr;
        return nullptr;
    }

    factory = getFactory();

    if (factory == nullptr)
    {
        error = {"It gave no plugin factory.", "GetPluginFactory", 0};
        native->exit();
        return nullptr;
    }

    return native;
}

void Vst3Module::exit(Native& native)
{
    // Exited at the program's end, never unloaded: its code may still be referred to by what it registered.
    if (native.exit != nullptr)
        native.exit();
}

}
