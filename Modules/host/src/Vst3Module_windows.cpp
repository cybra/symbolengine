#include "Vst3Module.h"

#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
    #define NOMINMAX
#endif
#include <windows.h>

#include <string>

namespace symbolengine
{

struct Vst3Module::Native
{
    HMODULE module = nullptr;
    bool (*exit)() = nullptr;
};

namespace
{

std::string systemMessage(DWORD code)
{
    wchar_t* buffer = nullptr;
    const auto length =
        FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       nullptr, code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring message(buffer != nullptr ? buffer : L"", length);
    LocalFree(buffer);

    while (!message.empty() && (message.back() == L'\n' || message.back() == L'\r' || message.back() == L' '))
        message.pop_back();

    const auto utf8 = std::filesystem::path(message).u8string();
    return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
}

// The bundle's folder for this computer's architecture, as Steinberg's layout names it.
std::filesystem::path binaryIn(const std::filesystem::path& bundle)
{
#if defined(_M_ARM64EC)
    const wchar_t* folders[] = {L"arm64ec-win", L"arm64x-win", L"x86_64-win"};
#elif defined(_M_ARM64)
    const wchar_t* folders[] = {L"arm64-win", L"arm64x-win"};
#elif defined(_M_X64) || defined(__x86_64__)
    const wchar_t* folders[] = {L"x86_64-win"};
#else
    const wchar_t* folders[] = {L"x86-win"};
#endif

    for (const auto* folder : folders)
    {
        const auto binary = bundle / L"Contents" / folder / bundle.filename();
        std::error_code ignored;

        if (std::filesystem::is_regular_file(binary, ignored))
            return binary;
    }

    return {};
}

}

std::shared_ptr<Vst3Module::Native> Vst3Module::load(const std::filesystem::path& location,
                                                     Steinberg::IPluginFactory*& factory, PluginError& error)
{
    std::error_code ignored;
    const auto absolute = std::filesystem::absolute(location, ignored);
    const auto binary = std::filesystem::is_directory(absolute, ignored) ? binaryIn(absolute) : absolute;

    if (binary.empty())
    {
        error = {"Its bundle holds no code for this computer's kind of processor.", "load", 0};
        return nullptr;
    }

    // From its absolute path, with its own folder searched for the libraries it depends on; nothing every other plugin
    // and thread shares, the working folder or the program's library folders, is changed for it.
    const HMODULE module = LoadLibraryExW(binary.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);

    if (module == nullptr)
    {
        const auto code = GetLastError();
        const auto path = binary.u8string();
        error = {systemMessage(code) + " (" + std::string(reinterpret_cast<const char*>(path.data()), path.size()) +
                     ")",
                 "load", static_cast<std::int64_t>(code)};
        return nullptr;
    }

    using Entry = bool (*)();
    using Factory = Steinberg::IPluginFactory*(PLUGIN_API*) ();
    auto native = std::make_shared<Native>();
    native->module = module;
    const auto entry = reinterpret_cast<Entry>(reinterpret_cast<void*>(GetProcAddress(module, "InitDll")));
    native->exit = reinterpret_cast<Entry>(reinterpret_cast<void*>(GetProcAddress(module, "ExitDll")));
    const auto getFactory =
        reinterpret_cast<Factory>(reinterpret_cast<void*>(GetProcAddress(module, "GetPluginFactory")));

    if (getFactory == nullptr)
    {
        error = {"It has no plugin factory (GetPluginFactory).", "load", 0};
        return nullptr;
    }

    // Steinberg's own hosting treats the entry and exit as optional on Windows, and plugins without them exist.
    if (entry != nullptr && !entry())
    {
        error = {"Its entry function refused to start it.", "InitDll", 0};
        native->exit = nullptr;
        return nullptr;
    }

    factory = getFactory();

    if (factory == nullptr)
    {
        error = {"It gave no plugin factory.", "GetPluginFactory", 0};

        if (native->exit != nullptr)
            native->exit();

        return nullptr;
    }

    return native;
}

void Vst3Module::exit(Native& native)
{
    // Exited, never freed: a library freed runs its detach code, which can hang on threads the plugin left running.
    if (native.exit != nullptr)
        native.exit();
}

}
