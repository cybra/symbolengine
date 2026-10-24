#pragma once

#include <symbolengine/core/Value.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

/** Interfaces drawn as web pages in the system's own web view. */
namespace symbolengine::ui
{

/** A file a page loads, served from memory. */
struct Resource
{
    std::string bytes{};
    std::string mimeType{};   // such as "text/html"; empty to go by the file's name
};

/** A page's files by path, such as "/index.html" or "/images/knob.svg", or nothing for a path it doesn't have. */
using Resources = std::function<std::optional<Resource>(std::string_view path)>;

/** The MIME type for a file by its name's extension, such as "text/javascript" for "app.js"; "application/octet-stream"
    for one it doesn't know. */
std::string mimeTypeFor(std::string_view path);

/** A web page in the system's web view: WKWebView on a Mac, and WebView2 on Windows, which comes with Windows 11 and
    which Microsoft installs on Windows 10. Every function is called on the message thread.

    The page's files come from its Resources, starting with "/index.html", and the page reaches the native side
    through symbolengine.js, which every page is served:

        <script src="symbolengine.js"></script>

        const answer = await symbolengine.call("name", argument, ...);
        symbolengine.on("event", data => { ... });

    The native side holds everything it sends or runs until the page has loaded that script, so nothing sent early is
    lost. */
class WebView
{
public:
    struct Options
    {
        Resources resources{};

        /** The page's own name, such as a plugin's id, which becomes its address: pages with different names never
            share what they store, even in one program, and pages with the same name always do. Letters, digits,
            dots and hyphens; anything else becomes a hyphen. */
        std::string name = "page";

        /** Windows only: the folder WebView2 keeps its data in, which it must be able to write to. Empty for a folder
            named after the program that loaded it, in AppData\Roaming. */
        std::filesystem::path dataFolder{};

        bool developerTools = false;   // the browser's inspector, on the page's context menu

        /** On a Mac, Command-C, X, V, Z and A act on the page's text as they do in Safari, which a page in another
            program's window, such as a plugin's editor in a host, needs. A program whose own Edit menu has them turns
            this off, so the keys reach its menu. */
        bool editingKeys = true;
    };

    explicit WebView(Options options);
    ~WebView();

    WebView(const WebView&) = delete;
    WebView& operator=(const WebView&) = delete;

    /** Whether the system's web view could be made: Windows without the WebView2 runtime has none. */
    bool available() const noexcept;

    /** The native view to place in a window: an NSView on a Mac, an HWND on Windows. */
    void* nativeView() const noexcept;

    /** Whether the page has loaded symbolengine.js and is taking what the native side sends. */
    bool pageReady() const noexcept;

    /** Adds a function the page calls with symbolengine.call, given its arguments as an array. What it returns is what
        the call's promise gives the page; an exception it throws rejects the promise with the exception's message. */
    void expose(std::string name, std::function<Value(const Value& arguments)> function);

    /** A function's answer, given once, on the message thread, when it is known. */
    using Reply = std::function<void(const Value& answer)>;

    /** Adds a function whose answer comes later, as after a dialog: the page's promise waits until the function calls
        reply, which it may do before it returns or at any time after. Only the first answer counts, and one given
        after the web view has gone goes nowhere. */
    void exposeLater(std::string name, std::function<void(const Value& arguments, Reply reply)> function);

    /** Sends an event to the page's handlers for it. */
    void send(std::string_view event, const Value& data);

    /** Runs a script in the page. */
    void run(std::string script);

    /** Runs a script in the page and gives what its last expression came to, as JSON has it, or, if it threw, null and
        what went wrong. */
    void run(std::string script, std::function<void(const Value& result, const std::string& error)> done);

    /** Saves a picture of the page as it is drawn now to a PNG file, for checks run from a script, and then says
        whether it was written. On a Mac only for now; elsewhere it says false. */
    void snapshot(const std::filesystem::path& png, std::function<void(bool written)> done);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}
