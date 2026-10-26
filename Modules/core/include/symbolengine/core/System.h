#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace symbolengine::system
{

/** An environment variable's value as UTF-8, or nothing if it isn't set. */
std::optional<std::string> environmentVariable(std::string_view name);

/** Opens a web page in the user's default browser, saying whether the system took it. Only http and https addresses
    are opened, so text from a page or a file can't open a file or another program through it. */
bool openInBrowser(std::string_view url);

/** Ends this process at once with the code, running nothing more of it: no destructors, no functions registered to
    run at exit and, on Windows, no library's detach code, which can deadlock once the process's other threads were
    stopped holding a lock. For a child whose link to its program is lost, and for any end that mustn't wait on code
    the process loaded. */
[[noreturn]] void endNow(int code);

}
