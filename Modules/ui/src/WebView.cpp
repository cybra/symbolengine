#include <symbolengine/ui/WebView.h>

#include <symbolengine/core/Json.h>
#include <symbolengine/core/Text.h>

#include <choc/gui/choc_WebView.h>

#include <map>
#include <vector>

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

struct WebView::Impl
{
    explicit Impl(Options given) : options(std::move(given)) {}

    Options options;
    std::unique_ptr<choc::ui::WebView> view;
    std::map<std::string, std::function<Value(const Value&)>, std::less<>> functions;
    std::vector<std::string> waiting;   // scripts held until the page is ready
    bool ready = false;

    std::optional<choc::ui::WebView::Options::Resource> fetch(const std::string& requested) const
    {
        std::string path = requested.empty() || requested == "/" ? "/index.html" : requested;
        if (const auto query = path.find_first_of("?#"); query != std::string::npos)
            path.resize(query);

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

    // The one native function the page's script calls: a function's name, and its arguments as JSON. The reply is
    // JSON too, {"value": ...} or {"error": "..."}.
    std::string call(std::string_view name, std::string_view arguments)
    {
        try
        {
            if (name == "__ready")
            {
                ready = true;
                for (const auto& script : std::exchange(waiting, {}))
                    view->evaluateJavascript(script);
                return json::write(Object{{"value", nullptr}});
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

    void run(std::string script)
    {
        if (ready)
            view->evaluateJavascript(script);
        else
            waiting.push_back(std::move(script));
    }
};

WebView::WebView(Options options) : impl(std::make_unique<Impl>(std::move(options)))
{
    choc::ui::WebView::Options settings;
    settings.enableDebugMode = impl->options.developerTools;
    settings.acceptsFirstMouseClick = true;
    settings.transparentBackground = true;
    settings.fetchResource = [this](const std::string& path) { return impl->fetch(path); };

    impl->view = std::make_unique<choc::ui::WebView>(settings);
    if (!impl->view->loadedOK())
    {
        impl->view.reset();
        return;
    }

    impl->view->bind("__symbolengine_call", [this](const choc::value::ValueView& arguments) -> choc::value::Value {
        if (!arguments.isArray() || arguments.size() != 2)
            return choc::value::createString(json::write(Object{{"error", "the call wasn't made as expected"}}));
        return choc::value::createString(
            impl->call(arguments[0].getWithDefault<std::string>({}), arguments[1].getWithDefault<std::string>({})));
    });
}

WebView::~WebView() = default;

bool WebView::available() const noexcept
{
    return impl->view != nullptr;
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
    impl->functions.insert_or_assign(std::move(name), std::move(function));
}

void WebView::send(std::string_view event, const Value& data)
{
    run("window.__symbolengine_receive(" + json::write(Value(event)) + ", " + json::write(data) + ");");
}

void WebView::run(std::string script)
{
    if (impl->view != nullptr)
        impl->run(std::move(script));
}

}
