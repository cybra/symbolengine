#include "PluginFiles.h"

#include <symbolengine/core/Files.h>
#include <symbolengine/core/Paths.h>
#include <symbolengine/core/System.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <tuple>

#if defined(__APPLE__)
    #include <CoreFoundation/CoreFoundation.h>
    #include <Security/Security.h>
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

#if defined(__APPLE__)

// What a Mach-O binary's load commands name for one architecture: the libraries it links and the folders it searches
// for those named through @rpath.
struct MachOLinks
{
    std::vector<std::string> libraries;
    std::vector<std::string> searched;
};

// Reads a binary's load commands, whatever its headers claim: the commands past what they fit are never read.
MachOLinks linksOf(const std::filesystem::path& binary, const std::string& architecture)
{
    constexpr std::uint64_t mostCommands = 16 * 1024 * 1024;

    MachOLinks links;
    std::ifstream file(binary, std::ios::binary);
    const auto head = bytesOf(file, 0, 8);
    if (head.size() < 8)
        return links;

    // Where its code for the architecture begins, in a universal binary.
    std::uint64_t start = 0;
    const auto magic = number(head, 0, 4, true);
    if (magic == 0xCAFEBABE || magic == 0xCAFEBABF)
    {
        const auto count = number(head, 4, 4, true);
        const bool wide = magic == 0xCAFEBABF;
        const std::size_t entry = wide ? 32 : 20;
        if (count == 0 || count >= 30)
            return links;
        const auto table = bytesOf(file, 8, static_cast<std::size_t>(count) * entry);
        bool found = false;
        for (std::size_t i = 0; !found && i + entry <= table.size(); i += entry)
            if (machOCpu(number(table, i, 4, true)) == architecture)
            {
                start = wide ? number(table, i + 8, 8, true) : number(table, i + 8, 4, true);
                found = true;
            }
        if (!found)
            return links;
    }

    const auto header = bytesOf(file, start, 32);
    if (header.size() < 32 || number(header, 0, 4, false) != 0xFEEDFACF ||
        machOCpu(number(header, 4, 4, false)) != architecture)
        return links;
    const auto count = number(header, 16, 4, false);
    const auto size = number(header, 20, 4, false);
    if (size > mostCommands)
        return links;
    const auto commands = bytesOf(file, start + 32, static_cast<std::size_t>(size));

    std::size_t at = 0;
    for (std::uint64_t i = 0; i < count && at + 8 <= commands.size(); ++i)
    {
        const auto command = number(commands, at, 4, false);
        const auto length = static_cast<std::size_t>(number(commands, at + 4, 4, false));
        if (length < 8 || length > commands.size() - at)
            break;

        const bool library = command == 0xC || command == 0x80000018 || command == 0x8000001F || command == 0x20 ||
                             command == 0x80000023;
        if ((library || command == 0x8000001C) && length >= 12)
        {
            const auto offset = static_cast<std::size_t>(number(commands, at + 8, 4, false));
            if (offset < length)
            {
                const auto end = std::min(commands.find('\0', at + offset), at + length);
                auto text = commands.substr(at + offset, end - (at + offset));
                if (!text.empty())
                    (library ? links.libraries : links.searched).push_back(std::move(text));
            }
        }
        at += length;
    }
    return links;
}

// Whether a file's code for the architecture fails its signature, checked as the system checks code as it is used, the
// bundle it may lie in left out. Each file is checked once in a program, until it changes, as the libraries of many
// plugins are often one.
bool codeFails(const std::filesystem::path& file, const std::string& architecture)
{
    static std::mutex lock;
    static std::map<std::pair<std::filesystem::path, std::string>, std::tuple<std::uintmax_t, std::int64_t, bool>>
        known;

    std::error_code error;
    const auto size = std::filesystem::file_size(file, error);
    const auto modified = modifiedNanoseconds(file, error);
    const auto key = std::make_pair(file, architecture);
    {
        const std::lock_guard held(lock);
        if (const auto found = known.find(key);
            found != known.end() && std::get<0>(found->second) == size && std::get<1>(found->second) == modified)
            return std::get<2>(found->second);
    }

    const auto text = paths::toUtf8(file);
    const auto url = CFURLCreateFromFileSystemRepresentation(nullptr, reinterpret_cast<const UInt8*>(text.data()),
                                                             static_cast<CFIndex>(text.size()), false);
    const auto name = CFStringCreateWithCString(nullptr, architecture.c_str(), kCFStringEncodingUTF8);
    const void* keys[] = {kSecCodeAttributeArchitecture};
    const void* values[] = {name};
    const auto attributes =
        CFDictionaryCreate(nullptr, keys, values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

    SecStaticCodeRef code = nullptr;
    auto status = SecStaticCodeCreateWithPathAndAttributes(url, kSecCSDefaultFlags, attributes, &code);
    if (status == errSecSuccess)
        status = SecStaticCodeCheckValidity(code,
                                            static_cast<SecCSFlags>(kSecCSDoNotValidateResources) |
                                                static_cast<SecCSFlags>(kSecCSNoNetworkAccess),
                                            nullptr);
    if (code != nullptr)
        CFRelease(code);
    CFRelease(attributes);
    CFRelease(name);
    CFRelease(url);

    // An Info.plist that changed fails first and alone, though the system stops no code for it; a fault in the code of
    // such a bundle is left for the system's crash report to say, as one the file on disk doesn't show is.
    const bool fails = status == errSecCSSignatureFailed;
    const std::lock_guard held(lock);
    known.insert_or_assign(key, std::make_tuple(size, modified, fails));
    return fails;
}

// Where the system would find a library a binary names: through its folders for @rpath, beside the binary for
// @loader_path, or at its own path; nothing for one found through the program rather than the plugin, or by no path.
std::optional<std::filesystem::path> locate(const std::string& name, const std::filesystem::path& loader,
                                            const std::vector<std::filesystem::path>& searched)
{
    constexpr std::string_view rpath = "@rpath/", loaderPath = "@loader_path/";
    std::error_code error;
    if (name.starts_with(rpath))
    {
        for (const auto& folder : searched)
            if (const auto path = folder / paths::fromUtf8(name.substr(rpath.size()));
                std::filesystem::exists(path, error))
                return path;
        return std::nullopt;
    }
    if (name.starts_with(loaderPath))
        return loader.parent_path() / paths::fromUtf8(name.substr(loaderPath.size()));
    if (name.starts_with("/"))
        return paths::fromUtf8(name);
    return std::nullopt;
}

#endif

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

std::int64_t nanosecondsOf(std::filesystem::file_time_type time)
{
    // MSVC's file clock converts only through clock_cast, which libc++ doesn't have; libc++ and libstdc++ have to_sys.
#if defined(_MSC_VER)
    const auto system = std::chrono::clock_cast<std::chrono::system_clock>(time);
#else
    const auto system = std::chrono::file_clock::to_sys(time);
#endif
    return std::chrono::duration_cast<std::chrono::nanoseconds>(system.time_since_epoch()).count();
}

std::int64_t modifiedNanoseconds(const std::filesystem::path& file, std::error_code& error)
{
    const auto time = std::filesystem::last_write_time(file, error);
    return error ? 0 : nanosecondsOf(time);
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
                text[0] != '\0' && version != text.data())
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

std::optional<std::filesystem::path> changedAfterSigning([[maybe_unused]] const std::filesystem::path& binary)
{
#if defined(__APPLE__)
    constexpr std::size_t mostFiles = 512;

    const auto architectures = architecturesOf(binary);
    if (architectures.empty())
        return std::nullopt;
    const std::string own(system::architecture());
    const auto architecture =
        std::find(architectures.begin(), architectures.end(), own) != architectures.end() ? own : architectures.front();

    // Each file once by its real path, with the folders of those that led to it, which it searches after its own.
    struct Image
    {
        std::filesystem::path file;
        std::vector<std::filesystem::path> searched;
    };
    std::error_code error;
    const auto first = std::filesystem::canonical(binary, error);
    if (error)
        return std::nullopt;
    std::vector<Image> waiting{{first, {}}};
    std::set<std::filesystem::path> seen{first};

    while (!waiting.empty())
    {
        const auto image = std::move(waiting.back());
        waiting.pop_back();
        if (codeFails(image.file, architecture))
            return image.file;

        const auto links = linksOf(image.file, architecture);
        std::vector<std::filesystem::path> searched;
        for (const auto& folder : links.searched)
            if (const auto found = locate(folder, image.file, {}); found.has_value())
                searched.push_back(*found);
        searched.insert(searched.end(), image.searched.begin(), image.searched.end());

        for (const auto& library : links.libraries)
        {
            const auto found = locate(library, image.file, searched);
            if (!found)
                continue;
            const auto real = std::filesystem::canonical(*found, error);
            if (error)
                continue;
            const auto text = paths::toUtf8(real);
            if (text.starts_with("/System/") || text.starts_with("/usr/lib/") || seen.size() >= mostFiles)
                continue;
            if (seen.insert(real).second)
                waiting.push_back({real, searched});
        }
    }
#endif
    return std::nullopt;
}

}
