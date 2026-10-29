#include "PluginFiles.h"

#include <symbolengine/core/Files.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>

#if defined(__APPLE__)
    #include <CoreFoundation/CoreFoundation.h>
#elif defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#endif

namespace symbolengine::pluginFiles
{

namespace
{

// Up to the bytes asked for from the file at the offset, fewer at its end.
std::string bytesOf(std::ifstream& file, std::uint64_t offset, std::size_t count)
{
    std::string bytes(count, '\0');
    file.clear();
    file.seekg(static_cast<std::streamoff>(offset));
    file.read(bytes.data(), static_cast<std::streamsize>(count));
    bytes.resize(static_cast<std::size_t>(std::max<std::streamsize>(0, file.gcount())));
    return bytes;
}

std::uint64_t number(const std::string& bytes, std::size_t at, std::size_t size, bool bigEndian)
{
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < size; ++i)
    {
        const auto byte = static_cast<unsigned char>(bytes[bigEndian ? at + i : at + size - 1 - i]);
        value = (value << 8) | byte;
    }
    return value;
}

std::string machOCpu(std::uint64_t cpu)
{
    switch (cpu)
    {
        case 0x01000007: return "x86_64";
        case 0x0100000C: return "arm64";
        case 7: return "x86";
        case 12: return "arm";
        default: return "other";
    }
}

void add(std::vector<std::string>& architectures, const std::string& architecture)
{
    if (std::find(architectures.begin(), architectures.end(), architecture) == architectures.end())
        architectures.push_back(architecture);
}

}

std::vector<std::string> architecturesOf(const std::filesystem::path& binary)
{
    std::ifstream file(binary, std::ios::binary);
    const auto head = bytesOf(file, 0, 64);
    std::vector<std::string> architectures;
    if (head.size() < 8)
        return architectures;

    const auto magic = number(head, 0, 4, true);

    // A thin Mach-O, in either byte order.
    if (magic == 0xFEEDFACE || magic == 0xFEEDFACF)
        add(architectures, machOCpu(number(head, 4, 4, true)));
    else if (magic == 0xCEFAEDFE || magic == 0xCFFAEDFE)
        add(architectures, machOCpu(number(head, 4, 4, false)));

    // A universal binary, whose count tells it from a Java class file, which begins the same.
    else if (magic == 0xCAFEBABE || magic == 0xCAFEBABF)
    {
        const auto count = number(head, 4, 4, true);
        const std::size_t entry = magic == 0xCAFEBABE ? 20 : 32;
        if (count > 0 && count < 30)
        {
            const auto table = bytesOf(file, 8, static_cast<std::size_t>(count) * entry);
            for (std::size_t i = 0; i + entry <= table.size(); i += entry)
                add(architectures, machOCpu(number(table, i, 4, true)));
        }
    }

    // Windows: the PE header's machine, where the DOS header says the PE header is.
    else if (head[0] == 'M' && head[1] == 'Z' && head.size() >= 0x40)
    {
        const auto at = number(head, 0x3C, 4, false);
        const auto pe = bytesOf(file, at, 6);
        if (pe.size() == 6 && pe.compare(0, 4, std::string("PE\0\0", 4)) == 0)
            switch (number(pe, 4, 2, false))
            {
                case 0x8664: add(architectures, "x86_64"); break;
                case 0xAA64: add(architectures, "arm64"); break;
                case 0x14C: add(architectures, "x86"); break;
                case 0x1C4: add(architectures, "arm"); break;
                default: add(architectures, "other"); break;
            }
    }

    // Linux: the ELF header's machine, in the byte order it says.
    else if (head.compare(0, 4,
                          "\x7F"
                          "ELF") == 0 &&
             head.size() >= 20)
        switch (number(head, 18, 2, head[5] == 2))
        {
            case 62: add(architectures, "x86_64"); break;
            case 183: add(architectures, "arm64"); break;
            case 3: add(architectures, "x86"); break;
            case 40: add(architectures, "arm"); break;
            default: add(architectures, "other"); break;
        }

    return architectures;
}

std::int64_t modifiedNanoseconds(const std::filesystem::path& file, std::error_code& error)
{
    const auto time = std::filesystem::last_write_time(file, error);
    if (error)
        return 0;
    const auto system = std::chrono::file_clock::to_sys(time);
    return std::chrono::duration_cast<std::chrono::nanoseconds>(system.time_since_epoch()).count();
}

std::string declaredVersion([[maybe_unused]] const std::filesystem::path& bundle,
                            [[maybe_unused]] const std::filesystem::path& binary)
{
#if defined(__APPLE__)
    // Read from the file itself, as CFBundle keeps what it read of a bundle for as long as the program runs.
    std::error_code error;
    const auto plist = files::read(bundle / "Contents" / "Info.plist", error);
    if (error || plist.empty())
        return {};
    const auto data =
        CFDataCreate(nullptr, reinterpret_cast<const UInt8*>(plist.data()), static_cast<CFIndex>(plist.size()));
    const auto read = CFPropertyListCreateWithData(nullptr, data, kCFPropertyListImmutable, nullptr, nullptr);
    CFRelease(data);
    if (read == nullptr)
        return {};
    std::string version;
    if (CFGetTypeID(read) == CFDictionaryGetTypeID())
        for (const auto key : {CFSTR("CFBundleShortVersionString"), CFSTR("CFBundleVersion")})
        {
            const auto value = CFDictionaryGetValue(static_cast<CFDictionaryRef>(read), key);
            std::array<char, 256> text{};
            if (value != nullptr && CFGetTypeID(value) == CFStringGetTypeID() &&
                CFStringGetCString(static_cast<CFStringRef>(value), text.data(), text.size(), kCFStringEncodingUTF8) &&
                text[0] != '\0')
                version += (version.empty() ? "" : " ") + std::string(text.data());
        }
    CFRelease(read);
    return version;
#elif defined(_WIN32)
    DWORD ignored = 0;
    const auto size = GetFileVersionInfoSizeW(binary.c_str(), &ignored);
    if (size == 0)
        return {};
    std::vector<char> information(size);
    VS_FIXEDFILEINFO* fixed = nullptr;
    UINT length = 0;
    if (!GetFileVersionInfoW(binary.c_str(), 0, size, information.data()) ||
        !VerQueryValueW(information.data(), L"\\", reinterpret_cast<void**>(&fixed), &length) || fixed == nullptr ||
        length < sizeof(VS_FIXEDFILEINFO))
        return {};
    return std::to_string(HIWORD(fixed->dwFileVersionMS)) + "." + std::to_string(LOWORD(fixed->dwFileVersionMS)) + "." +
           std::to_string(HIWORD(fixed->dwFileVersionLS)) + "." + std::to_string(LOWORD(fixed->dwFileVersionLS));
#else
    return {};
#endif
}

}
