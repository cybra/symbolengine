#include "SystemCrashReports.h"

#include <symbolengine/core/Files.h>
#include <symbolengine/core/Json.h>
#include <symbolengine/core/Paths.h>

#include <cstdlib>
#include <string_view>
#include <utility>

namespace symbolengine::systemCrashReports
{

namespace
{

// The file the report's fault lies in, among the images it lists: "UNKNOWN_0x32 at 0x000000011bbf424c".
std::string fileAtFault(const Value& report)
{
    constexpr std::string_view at = " at 0x";
    const auto subtype = report["exception"]["subtype"].stringOr("");
    const auto found = subtype.find(at);
    if (found == std::string::npos)
        return {};
    const auto address = std::strtoull(subtype.c_str() + found + at.size(), nullptr, 16);

    const auto& images = report["usedImages"];
    if (!images.isArray())
        return {};
    for (const auto& image : images.asArray())
    {
        const auto base = static_cast<std::uint64_t>(image["base"].intOr(0));
        const auto size = static_cast<std::uint64_t>(image["size"].intOr(0));
        if (address >= base && address - base < size)
            return image["path"].stringOr("");
    }
    return {};
}

}

Reader::Reader(std::filesystem::path from, std::filesystem::file_time_type after)
    : folder(std::move(from)), since(after)
{
}

std::optional<Report> Reader::find(std::int64_t process)
{
    std::error_code error;
    std::filesystem::directory_iterator entries(folder, error);
    for (; !error && entries != std::filesystem::directory_iterator(); entries.increment(error))
    {
        const auto path = entries->path();
        std::error_code unread;
        if (path.extension() != ".ips" || others.contains(path) || entries->last_write_time(unread) < since || unread)
            continue;

        // A line of its header, then the report.
        const auto text = files::read(path, unread);
        const auto line = text.find('\n');
        if (unread || line == std::string::npos)
            continue;
        const auto report = json::tryParse(std::string_view(text).substr(line + 1));
        if (!report || !report->isObject())
            continue;
        if ((*report)["pid"].intOr(-1) != process)
        {
            others.insert(path);
            continue;
        }

        Report found;
        found.codeSignature = (*report)["termination"]["namespace"].stringOr("") == "CODESIGNING";
        found.file = fileAtFault(*report);
        return found;
    }
    return std::nullopt;
}

std::filesystem::path systemFolder()
{
    std::error_code error;
    return paths::systemFolder(paths::Folder::home, error) / "Library" / "Logs" / "DiagnosticReports";
}

}
