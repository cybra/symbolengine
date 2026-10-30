#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <set>
#include <string>

// What the system's crash reports say of a program it stopped, on a Mac, where a signal alone can't tell a kill for a
// program's code from any other.
namespace symbolengine::systemCrashReports
{

/** What a process's report says stopped it: whether it was code that no longer matches its signature, and the file that
    code lies in, as the report names it, if it says. */
struct Report
{
    bool codeSignature = false;
    std::string file{};
};

/** The reports in a folder written since a time, read as they come. */
class Reader
{
public:
    Reader(std::filesystem::path folder, std::filesystem::file_time_type since);

    /** The report of a process with this number, or nothing while there is none, whole, to read. A report still being
        written is read again next time, and those of other processes aren't. */
    std::optional<Report> find(std::int64_t process);

private:
    std::filesystem::path folder;
    std::filesystem::file_time_type since;
    std::set<std::filesystem::path> others;
};

/** The system's own folder of them. */
std::filesystem::path systemFolder();

}
