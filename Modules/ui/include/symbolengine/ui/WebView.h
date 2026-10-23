#pragma once

#include <symbolengine/core/Value.h>

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
        bool developerTools = false;   // the browser's inspector, on the page's context menu
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

    /** Sends an event to the page's handlers for it. */
    void send(std::string_view event, const Value& data);

    /** Runs a script in the page. */
    void run(std::string script);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}
