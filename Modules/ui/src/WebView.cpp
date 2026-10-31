#include <symbolengine/ui/WebView.h>

#include <symbolengine/core/Json.h>
#include <symbolengine/core/Paths.h>
#include <symbolengine/core/System.h>
#include <symbolengine/core/Text.h>
#include <symbolengine/ui/MessageLoop.h>

#include <choc/gui/choc_WebView.h>
#include <choc/text/choc_JSON.h>

#include <map>
#include <memory>
#include <utility>
#include <vector>

namespace symbolengine::ui::detail
{
/** Saves a picture of the system's web view to a PNG, in Snapshot_mac.mm or Native_windows.cpp. */
void snapshot(void* view, const std::string& png, std::function<void(bool)> done);
}

// The bridge's script, built into the module from web/symbolengine.js.
std::optional<std::string_view> symbolengine_ui_bridge_files(std::string_view path);

namespace symbolengine::ui
{

std::string mimeTypeFor(std::string_view path)
{
    const auto dot = path.rfind('.');
    const auto extension =
        text::toLowerAscii(dot == std::string_view::npos ? std::string_view() : path.substr(dot + 1));

    static const std::map<std::string, std::string, std::less<>> types{
        {"html", "text/html"},   {"htm", "text/html"},         {"js", "text/javascript"}, {"mjs", "text/javascript"},
        {"css", "text/css"},     {"json", "application/json"}, {"svg", "image/svg+xml"},  {"png", "image/png"},
        {"jpg", "image/jpeg"},   {"jpeg", "image/jpeg"},       {"gif", "image/gif"},      {"webp", "image/webp"},
        {"ico", "image/x-icon"}, {"woff", "font/woff"},        {"woff2", "font/woff2"},   {"ttf", "font/ttf"},
        {"otf", "font/otf"},     {"wasm", "application/wasm"}, {"txt", "text/plain"},     {"wav", "audio/wav"},
        {"mp3", "audio/mpeg"},
    };

    const auto found = types.find(extension);
    return found != types.end() ? found->second : "application/octet-stream";
}

namespace
{
// A page's address from its name. A Mac serves it on a scheme of SymbolEngine's own; WebView2 serves it on a
// name under .localhost, which never reaches the network.
std::string addressFor(std::string_view name)
{
    std::string host;
    for (const char c : text::toLowerAscii(name))
        host += (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-' ? c : '-';
    if (host.empty())
        host = "page";

#if defined(_WIN32)
    return "https://" + host + ".localhost/";
#else
    return "symbolengine://" + host + "/";
#endif
}
}

#if defined(_WIN32)
namespace
{
std::string decodedPath(const std::string& path)
{
    const auto digit = [](char c) {
        return c >= '0' && c <= '9'   ? c - '0'
               : c >= 'a' && c <= 'f' ? c - 'a' + 10
               : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                      : -1;
    };

    std::string decoded;
    for (std::size_t i = 0; i < path.size(); ++i)
    {
        if (path[i] == '%' && i + 2 < path.size() && digit(path[i + 1]) >= 0 && digit(path[i + 2]) >= 0)
        {
            decoded += static_cast<char>(digit(path[i + 1]) * 16 + digit(path[i + 2]));
            i += 2;
        }
        else
        {
            decoded += path[i];
        }
    }
    return decoded;
}
}
#endif

struct WebView::Impl
{
    explicit Impl(Options given) : options(std::move(given)) {}

    Options options;
    std::unique_ptr<choc::ui::WebView> view;
    std::map<std::string, std::function<Value(const Value&)>, std::less<>> functions;
    std::map<std::string, std::function<void(const Value&, Reply)>, std::less<>> laterFunctions;
    std::vector<std::pair<std::string, choc::ui::WebView::CompletionHandler>> waiting;   // held until the page is ready
    bool ready = false;
    bool failed = false;   // WebView2 turned out not to be able to start, or has gone

    // Held by each answer still to come, which goes nowhere once the web view has gone.
    std::shared_ptr<Impl*> self = std::make_shared<Impl*>(this);

    std::optional<choc::ui::WebView::Options::Resource> fetch(const std::string& requested) const
    {
        std::string path = requested.empty() || requested == "/" ? "/index.html" : requested;
        if (const auto query = path.find_first_of("?#"); query != std::string::npos)
            path.resize(query);

#if defined(_WIN32)
        // WebView2 gives the address as the page spelled it; a Mac's web view has already read it.
        (void) &decodedPath;
#endif

        if (path == "/symbolengine.js")
        {
            const auto script = symbolengine_ui_bridge_files("/symbolengine.js");
            return choc::ui::WebView::Options::Resource(*script, "text/javascript");
        }

        if (!options.resources)
            return std::nullopt;
        const auto found = options.resources(path);
        if (!found)
            return std::nullopt;
        return choc::ui::WebView::Options::Resource(found->bytes,
                                                    found->mimeType.empty() ? mimeTypeFor(path) : found->mimeType);
    }

    // The one native function the page's script calls: a function's name, its arguments as JSON, and the call's number.
    // The reply is JSON too, {"value": ...} or {"error": "..."}, or {"later": true} for an answer that comes later as
    // the same, through window.__symbolengine_answer with the call's number.
    std::string call(std::string_view name, std::string_view arguments, std::int64_t number)
    {
        try
        {
            if (name == "__ready")
            {
                ready = true;
                for (auto& [script, done] : std::exchange(waiting, {}))
                    view->evaluateJavascript(script, std::move(done));
                return json::write(Object{{"value", nullptr}});
            }

            if (name == "__openLink")
                return json::write(Object{{"value", system::openInBrowser(json::parse(arguments)[0].stringOr(""))}});

            if (const auto later = laterFunctions.find(name); later != laterFunctions.end())
            {
                std::weak_ptr<Impl*> alive = self;
                auto answered = std::make_shared<bool>(false);
                later->second(json::parse(arguments), [alive, answered, number](const Value& answer) {
                    const auto impl = alive.lock();
                    if (impl == nullptr || std::exchange(*answered, true))
                        return;

                    // Not held for the page's readiness, since the page made the call and so is listening, and may be
                    // waiting for this answer before it is ready.
                    (*impl)->view->evaluateJavascript("window.__symbolengine_answer(" + json::write(Value(number)) + ", " +
                                                      json::write(Object{{"value", answer}}) + ");");
                });
                return json::write(Object{{"later", true}});
            }

            const auto function = functions.find(name);
            if (function == functions.end())
                throw std::runtime_error("there is no native function called " + std::string(name));

            return json::write(Object{{"value", function->second(json::parse(arguments))}});
        }
        catch (const std::exception& error)
        {
            return json::write(Object{{"error", error.what()}});
        }
    }

    void run(std::string script, choc::ui::WebView::CompletionHandler done = {})
    {
        if (failed)
            return;

        if (ready)
            view->evaluateJavascript(script, std::move(done));
        else
            waiting.emplace_back(std::move(script), std::move(done));
    }
};

WebView::WebView(Options options) : impl(std::make_unique<Impl>(std::move(options)))
{
    // On the thread making it, the message thread, whatever thread asked for a call first.
    choc::ui::WebView::Options settings;
    settings.enableDebugMode = impl->options.developerTools;
    settings.enableDefaultClipboardKeyShortcutsInSafari = impl->options.editingKeys;
    settings.acceptsFirstMouseClick = true;
    settings.transparentBackground = true;
    settings.fetchResource = [this](const std::string& path) { return impl->fetch(path); };
    settings.shouldAllowRequest = [this](const choc::ui::WebView::Options::Request& request) {
        return impl->options.allowAddress && impl->options.allowAddress(request.uri);
    };
    settings.webviewFailed = [this] {
        impl->failed = true;
        for (auto& [script, done] : std::exchange(impl->waiting, {}))
            if (done)
                done("there is no web view", {});
    };
    // What is sent meanwhile waits for the new page.
    settings.webviewRendererLost = [this] { impl->ready = false; };
    settings.customSchemeURI = addressFor(impl->options.name);
    settings.userDataFolder = paths::toUtf8(impl->options.dataFolder);
#if defined(_WIN32)
    if (impl->options.background.has_value())
    {
        settings.transparentBackground = false;
        settings.windowsBackgroundColor = *impl->options.background;
    }
#endif

    // WebView2 takes a binding only once it has started, which may be after the page has begun loading, so the
    // native function is bound when the view says it is ready; on a Mac that is while it is being made.
    settings.webviewIsReady = [this](choc::ui::WebView& view) {
        view.bind("__symbolengine_call", [this](const choc::value::ValueView& arguments) -> choc::value::Value {
            if (!arguments.isArray() || arguments.size() != 3)
                return choc::value::createString(json::write(Object{{"error", "the call wasn't made as expected"}}));
            return choc::value::createString(impl->call(arguments[0].getWithDefault<std::string>({}),
                                                        arguments[1].getWithDefault<std::string>({}),
                                                        arguments[2].getWithDefault<std::int64_t>(0)));
        });
    };

    impl->view = std::make_unique<choc::ui::WebView>(settings);
    if (!impl->view->loadedOK())
    {
        impl->view.reset();
        return;
    }
}

WebView::~WebView() = default;

bool WebView::available() const noexcept
{
    return impl->view != nullptr && !impl->failed;
}

void* WebView::nativeView() const noexcept
{
    return impl->view != nullptr ? impl->view->getViewHandle() : nullptr;
}

bool WebView::pageReady() const noexcept
{
    return impl->ready;
}

void WebView::expose(std::string name, std::function<Value(const Value& arguments)> function)
{
    impl->laterFunctions.erase(name);
    impl->functions.insert_or_assign(std::move(name), std::move(function));
}

void WebView::exposeLater(std::string name, std::function<void(const Value& arguments, Reply reply)> function)
{
    impl->functions.erase(name);
    impl->laterFunctions.insert_or_assign(std::move(name), std::move(function));
}

void WebView::send(std::string_view event, const Value& data)
{
    run("window.__symbolengine_receive(" + json::write(Value(event)) + ", " + json::write(data) + ");");
}

void WebView::snapshot(const std::filesystem::path& png, std::function<void(bool)> done)
{
    if (impl->view == nullptr)
        done(false);
    else
        detail::snapshot(impl->view->getViewHandle(), paths::toUtf8(png), std::move(done));
}

void WebView::run(std::string script)
{
    if (impl->view != nullptr)
        impl->run(std::move(script));
}

void WebView::run(std::string script, std::function<void(const Value& result, const std::string& error)> done)
{
    if (!available())
    {
        done(nullptr, "there is no web view");
        return;
    }

    impl->run(std::move(script), [done = std::move(done)](const std::string& error, const choc::value::ValueView& result) {
        Value value;
        if (error.empty() && !result.isVoid())
            value = json::parse(choc::json::toString(result));
        done(value, error);
    });
}

}
