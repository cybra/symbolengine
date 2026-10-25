#pragma once

#include <symbolengine/app/Application.h>

#include <filesystem>
#include <string>
#include <vector>

/** What each system does for an Application, written once for a Mac and once for Windows. */
namespace symbolengine::app::platform
{

/** Makes the system's application object ready, before any window: on a Mac the shared application and its delegate,
    which passes the system's requests to the Application; on Windows a hidden window the system and other instances
    reach the program through, the program's output joined to the console it was started from, and its windows made
    aware of each screen's scaling. */
void start(Application& application, const std::string& identifier);
void finish();

std::vector<std::string> arguments(int argc, char** argv);

/** Finds an instance already running under the identifier and hands it the arguments, or becomes the running instance
    and returns false. */
bool sendToRunningInstance(const std::string& identifier, const std::vector<std::string>& arguments);

bool isForeground();
void setDockIconVisible(bool visible);
void noteRecentFile(const std::filesystem::path& file);
[[noreturn]] void terminate(int code);

}
