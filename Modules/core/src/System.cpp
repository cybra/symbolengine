#include "symbolengine/core/System.h"

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>

    #include <filesystem>
    #include <vector>
#else
    #include <cstdlib>
#endif

namespace symbolengine::system
{

std::optional<std::string> environmentVariable(std::string_view name)
{
#if defined(_WIN32)
    // Windows keeps the environment in UTF-16; std::getenv would give it in the system's code page.
    const auto wide = std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(name.data()), name.size()));
    const auto needed = GetEnvironmentVariableW(wide.c_str(), nullptr, 0);
    if (needed == 0)
        return std::nullopt;

    std::vector<wchar_t> value(needed);
    const auto length = GetEnvironmentVariableW(wide.c_str(), value.data(), needed);
    const auto text = std::filesystem::path(std::wstring(value.data(), length)).u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
#else
    if (const char* value = std::getenv(std::string(name).c_str()))
        return std::string(value);
    return std::nullopt;
#endif
}

}
