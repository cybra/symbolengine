#pragma once

#include <filesystem>
#include <string_view>

// A scan worker's crash note: the file and step it is on, written before each step to a file the program named for it,
// which a crash adds its signal or exception and a backtrace to before the process ends as the crash would have ended
// it, so the program can say which plugin crashed, how and where.
namespace symbolengine::scanWorkerCrash
{

/** Opens the note and installs the crash handler, at the worker's start. */
void begin(const std::filesystem::path& note);

/** Replaces what the note says, before each step. */
void note(std::string_view text);

}
