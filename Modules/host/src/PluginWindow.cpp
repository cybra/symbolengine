#include "symbolengine/host/PluginWindow.h"
#include "symbolengine/host/PluginWindows.h"

#include "PluginWindowNative.h"

#include <symbolengine/ui/Native.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <string_view>

// SymbolEngine's own page of parameters, built into the module from its web folder.
std::optional<std::string_view> symbolengine_host_parameter_page(std::string_view path);

namespace symbolengine
{

namespace
{
constexpr ui::Size smallest{100, 50};

// The parameters a page shows: all but the hidden ones and the plugin's own bypass, which the program's switch stands
// in for.
std::vector<PluginParameter> shownOf(const Plugin& plugin)
{
    auto parameters = plugin.parameters();
    std::erase_if(parameters, [](const PluginParameter& parameter) { return parameter.hidden || parameter.bypass; });
    return parameters;
}
}

// The window, and either the plugin's editor in a view of the window's own made with it and kept for its life, or
// the page of its parameters. An editor's sizes are its own: points on a Mac and pixels on Windows, the window
// measuring itself in pixels there.
struct PluginWindow::Impl final : PluginEditorFrame
{
    PluginWindow* owner = nullptr;
    PluginWindows* group = nullptr;
    std::shared_ptr<Plugin> plugin;
    std::unique_ptr<ui::Window> window;
    void* container = nullptr;
    std::unique_ptr<PluginEditor> editor;
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);

    // The page standing in for its editor, why, and what it was last shown.
    std::unique_ptr<ui::WebView> page;
    std::unique_ptr<ui::Timer> look;
    PluginError problem;
    std::vector<PluginParameter> shown;
    std::map<std::uint32_t, double> sent;

    ~Impl() override
    {
        // The editor goes while the window and its view are whole, then the window, then the view.
        *alive = false;
        editor.reset();
        look.reset();

        const pluginWindowNative::Awareness awareness;

        if (page != nullptr)
        {
            window->setContent(nullptr);
            page.reset();
        }

        window.reset();

        if (container != nullptr)
            pluginWindowNative::releaseContainer(container);
    }

    void place(PluginEditorSize size) override
    {
        const pluginWindowNative::Awareness awareness;
        window->setSize({size.width, size.height});
    }

    double scale() const override { return window->scale(); }

    // The window resized by the user, or by the program, told to the editor when it isn't the editor's own size.
    void movedOrResized()
    {
        if (editor != nullptr)
            if (const auto now = window->size(); PluginEditorSize{now.width, now.height} != editor->size())
                editor->resized({now.width, now.height});

        if (owner != nullptr && owner->onMovedOrResized)
            owner->onMovedOrResized();
    }

    // A drag of its edge held to the nearest size the editor takes, the size it has for one that can't be resized; a
    // page takes any.
    ui::Size resizing(ui::Size wanted) const
    {
        if (editor == nullptr)
            return page != nullptr ? wanted : window->size();

        const auto taken = editor->constrain({wanted.width, wanted.height});
        return {taken.width, taken.height};
    }

    void rescaled()
    {
        if (editor != nullptr)
            editor->rescaled(window->scale());
    }

    // The page's functions: the parameters shown, each step's text, and a gesture's begin, values and end, each
    // reaching the plugin as its own editor's edit would.
    void expose()
    {
        const auto idOf = [](const Value& arguments) { return static_cast<std::uint32_t>(arguments[0].intOr(0)); };
        const auto writable = [this](std::uint32_t id) {
            const auto found = std::find_if(shown.begin(), shown.end(),
                                            [id](const PluginParameter& parameter) { return parameter.id == id; });
            return found != shown.end() && !found->readOnly;
        };

        page->expose("parameters", [this](const Value&) {
            shown = shownOf(*plugin);
            Array list;

            for (const auto& parameter : shown)
            {
                const auto value = plugin->parameter(parameter.id);
                sent[parameter.id] = value;
                list.push_back(Object{{"id", parameter.id},
                                      {"name", parameter.name},
                                      {"units", parameter.units},
                                      {"steps", parameter.steps},
                                      {"value", value},
                                      {"text", plugin->parameterText(parameter.id, value)},
                                      {"readOnly", parameter.readOnly}});
            }

            return Value(std::move(list));
        });

        page->expose("stepTexts", [this, idOf](const Value& arguments) {
            const auto id = idOf(arguments);
            Array texts;

            for (const auto& parameter : shown)
                if (parameter.id == id)
                    for (int step = 0; step <= std::min(parameter.steps, 128); ++step)
                        texts.push_back(plugin->parameterText(id, static_cast<double>(step) / parameter.steps));

            return Value(std::move(texts));
        });

        page->expose("begin", [this, idOf, writable](const Value& arguments) {
            if (writable(idOf(arguments)))
                plugin->editAsEditor({PluginEdit::Kind::begin, idOf(arguments), 0.0});
            return Value();
        });

        page->expose("set", [this, idOf, writable](const Value& arguments) {
            const auto id = idOf(arguments);
            const auto value = std::clamp(arguments[1].numberOr(0.0), 0.0, 1.0);

            if (!writable(id))
                return Value(plugin->parameterText(id, plugin->parameter(id)));

            plugin->editAsEditor({PluginEdit::Kind::value, id, value});
            sent[id] = value;
            return Value(plugin->parameterText(id, value));
        });

        page->expose("end", [this, idOf, writable](const Value& arguments) {
            if (writable(idOf(arguments)))
                plugin->editAsEditor({PluginEdit::Kind::end, idOf(arguments), 0.0});
            return Value();
        });
    }

    // Ten times a second: parameters changed told to the page, which asks for them again, or else each value changed
    // elsewhere, by the program, its automation or the plugin itself, with its text.
    void lookAtValues()
    {
        if (plugin->isBusy() || plugin->state() == Plugin::State::failed || !page->pageReady())
            return;

        if (const auto now = shownOf(*plugin); now != shown)
        {
            shown = now;
            sent.clear();
            page->send("parameters", Value());
            return;
        }

        Array changed;

        for (const auto& parameter : shown)
        {
            const auto value = plugin->parameter(parameter.id);

            if (const auto last = sent.find(parameter.id); last != sent.end() && last->second == value)
                continue;

            sent[parameter.id] = value;
            changed.push_back(
                Object{{"id", parameter.id}, {"value", value}, {"text", plugin->parameterText(parameter.id, value)}});
        }

        if (!changed.empty())
            page->send("values", Value(std::move(changed)));
    }
};

std::unique_ptr<PluginWindow> PluginWindow::open(std::shared_ptr<Plugin> plugin, Options options, PluginError& error)
{
    error = {};

    if (plugin == nullptr)
    {
        error = {"There is no plugin.", "openEditor", 0};
        return nullptr;
    }

    if (plugin->isBusy())
    {
        error = {"A call into it is under way.", "openEditor", 0};
        return nullptr;
    }

    if (plugin->state() == Plugin::State::failed)
    {
        error = plugin->problem();
        return nullptr;
    }

    pluginWindowNative::prepare();

    // Its editor tried first, as asking whether it has one makes a view of its own.
    if (auto made = openEditor(plugin, options, error))
        return made;

    // Refused by the host's own rules, as when one of its editors is open: no page in its place.
    if (!options.parameterPage || plugin->editorOpen() || plugin->isBusy() || plugin->state() == Plugin::State::failed)
        return nullptr;

    const auto problem = error;
    return openPage(std::move(plugin), options, problem, error);
}

std::unique_ptr<PluginWindow> PluginWindow::openEditor(std::shared_ptr<Plugin> plugin, const Options& options,
                                                       PluginError& error)
{
    auto parts = std::make_unique<Impl>();
    parts->plugin = plugin;

    {
        const pluginWindowNative::Awareness awareness;
        parts->window = std::make_unique<ui::Window>(
            ui::Window::Options{options.title, {400, 300}, smallest, true, false, options.background, true});
        parts->container =
            pluginWindowNative::makeContainer(*parts->window, [state = parts.get()](char32_t key, bool repeat) {
                // A key the plugin's view handed up, taken here: the program's, where the window has joined its others.
                if (state->group != nullptr && state->owner != nullptr)
                    state->group->handedUp(*state->owner, key, repeat);
            });
    }

    if (parts->container == nullptr)
    {
        error = {"The window's view for its editor couldn't be made.", "openEditor", 0};
        return nullptr;
    }

    parts->window->setContent(parts->container);
    parts->editor = PluginEditor::open(std::move(plugin), parts->container, *parts, error);

    if (parts->editor == nullptr)
        return nullptr;

    {
        const pluginWindowNative::Awareness awareness;
        parts->window->setResizable(parts->editor->resizable());
        parts->window->center();
    }

    pluginWindowNative::settle(parts->container, parts->alive);
    auto& state = *parts;
    return finish(std::unique_ptr<PluginWindow>(new PluginWindow(std::move(parts))), state, options.windows);
}

// The page in a window measured as the program's own pages are, at its own size, resizable.
std::unique_ptr<PluginWindow> PluginWindow::openPage(std::shared_ptr<Plugin> plugin, const Options& options,
                                                     PluginError problem, PluginError& error)
{
    auto parts = std::make_unique<Impl>();
    parts->plugin = std::move(plugin);
    parts->problem = std::move(problem);

    {
        const pluginWindowNative::Awareness awareness;
        parts->window = std::make_unique<ui::Window>(
            ui::Window::Options{options.title, {480, 420}, {320, 160}, true, false, options.background, false});
    }

    ui::Resources resources = options.page;

    if (resources == nullptr)
        resources = [](std::string_view path) -> std::optional<ui::Resource> {
            const auto bytes = symbolengine_host_parameter_page(path);
            return bytes ? std::optional<ui::Resource>(ui::Resource{std::string(*bytes), ""}) : std::nullopt;
        };

    parts->page = std::make_unique<ui::WebView>(
        ui::WebView::Options{std::move(resources), "symbolengine-parameters", {}, false, options.background, true});

    if (!parts->page->available())
    {
        error = {"The page of its parameters can't be shown: the system's web view isn't there.", "openEditor", 0};
        return nullptr;
    }

    parts->shown = shownOf(*parts->plugin);
    parts->expose();
    parts->window->setContent(parts->page->nativeView());

    {
        const pluginWindowNative::Awareness awareness;
        parts->window->center();
    }

    auto& state = *parts;
    parts->look = std::make_unique<ui::Timer>(100, [&state] { state.lookAtValues(); });
    return finish(std::unique_ptr<PluginWindow>(new PluginWindow(std::move(parts))), state, options.windows);
}

// The window made, its callbacks set and joined to the program's others.
std::unique_ptr<PluginWindow> PluginWindow::finish(std::unique_ptr<PluginWindow> made, Impl& state,
                                                   PluginWindows* group)
{
    auto* window = made.get();
    state.owner = window;
    state.group = group;
    auto* parts = &state;
    state.window->onMovedOrResized = [parts] { parts->movedOrResized(); };
    state.window->onResizing = [parts](ui::Size wanted) { return parts->resizing(wanted); };
    state.window->onScaleChanged = [parts] { parts->rescaled(); };
    state.window->onCloseButton = [window] {
        if (auto pressed = window->onCloseButton)
            pressed();
    };
    state.window->onActiveChanged = [window](bool active) {
        if (window->onActiveChanged)
            window->onActiveChanged(active);
    };

    if (group != nullptr)
        group->join(*window);

    return made;
}

PluginWindow::PluginWindow(std::unique_ptr<Impl> made) : impl(std::move(made)) {}

PluginWindow::~PluginWindow()
{
    if (impl->group != nullptr)
        impl->group->leave(*this);

    std::shared_ptr<Impl> closing(std::move(impl));
    closing->owner = nullptr;
    closing->window->onCloseButton = nullptr;
    closing->window->onActiveChanged = nullptr;

    // During a call into the plugin, as one its own dialog lets run: hidden now, and closed once the call returns.
    if (closing->plugin->isBusy())
    {
        closing->window->setVisible(false);
        const auto plugin = closing->plugin;
        plugin->whenFree([closing] {});
    }
}

const std::shared_ptr<Plugin>& PluginWindow::plugin() const
{
    return impl->plugin;
}

PluginEditorSize PluginWindow::editorSize() const
{
    if (impl->editor != nullptr)
        return impl->editor->size();

    const auto size = impl->window->size();
    return {size.width, size.height};
}

PluginEditor* PluginWindow::editor() const
{
    return impl->editor.get();
}

ui::WebView* PluginWindow::parameterPage() const
{
    return impl->page.get();
}

PluginError PluginWindow::editorProblem() const
{
    return impl->problem;
}

bool PluginWindow::isResizable() const
{
    return impl->window->isResizable();
}

void PluginWindow::setTitle(const std::string& title)
{
    impl->window->setTitle(title);
}

void PluginWindow::setVisible(bool visible)
{
    impl->window->setVisible(visible);
}

bool PluginWindow::isVisible() const
{
    return impl->window->isVisible();
}

void PluginWindow::toFront()
{
    impl->window->toFront();
}

bool PluginWindow::isActive() const
{
    return impl->window->isActive();
}

void PluginWindow::setAlwaysOnTop(bool onTop)
{
    impl->window->setAlwaysOnTop(onTop);
}

ui::Bounds PluginWindow::bounds() const
{
    return impl->window->bounds();
}

void PluginWindow::moveTo(int x, int y)
{
    const pluginWindowNative::Awareness awareness;
    const auto size = impl->window->size();
    impl->window->setBounds({x, y, size.width, size.height});
}

void PluginWindow::center()
{
    const pluginWindowNative::Awareness awareness;
    impl->window->center();
}

void* PluginWindow::nativeHandle() const
{
    return impl->window->nativeHandle();
}

}
