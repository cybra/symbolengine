#include "Editor.h"

#include "Access.h"
#include "TextValues.h"

#include <stdexcept>
#include <string>

// plugin.js, built into the module from web/plugin.js.
std::optional<std::string_view> symbolengine_plugin_page_files(std::string_view path);

namespace symbolengine::plugin::detail
{

namespace
{
// How a stepped parameter with this many steps or fewer lists them for the page, as a menu would.
constexpr double mostSteps = 64.0;

Value describe(const Parameter& parameter, double value)
{
    Object about{{"id", parameter.id},
                 {"name", parameter.name},
                 {"group", parameter.group},
                 {"minimum", parameter.minimum},
                 {"maximum", parameter.maximum},
                 {"default", parameter.defaultValue},
                 {"stepped", parameter.stepped},
                 {"automatable", parameter.automatable},
                 {"bypass", parameter.bypass},
                 {"value", value},
                 {"text", textFor(parameter, value)}};

    if (parameter.stepped && parameter.maximum - parameter.minimum <= mostSteps)
    {
        Array steps;
        for (double step = parameter.minimum; step <= parameter.maximum; step += 1.0)
            steps.push_back(Object{{"value", step}, {"text", textFor(parameter, step)}});
        about.set("steps", std::move(steps));
    }
    return about;
}

ui::WebView::Options pageOptions(const Editor& editor)
{
    return {.resources = [files = editor.files](std::string_view path) -> std::optional<ui::Resource> {
        if (path == "/plugin.js")
            return ui::Resource{std::string(*symbolengine_plugin_page_files("/plugin.js")), "text/javascript"};
        if (!files)
            return std::nullopt;
        const auto found = files(path);
        if (!found)
            return std::nullopt;
        return ui::Resource{std::string(*found), ""};
    }};
}
}

EditorSession::EditorSession(Plugin& owner, Editor description, EditorChanges& queue, std::function<void()> tell)
    : plugin(owner), editor(std::move(description)), changes(queue), told(std::move(tell)), view(pageOptions(editor))
{
    const auto& parameters = plugin.parameters();
    for (std::size_t i = 0; i < parameters.size(); ++i)
        sent.push_back(Access::value(plugin, i));

    view.expose("__parameters", [this](const Value&) {
        Array all;
        for (std::size_t i = 0; i < plugin.parameters().size(); ++i)
        {
            sent[i] = Access::value(plugin, i);
            all.push_back(describe(plugin.parameters()[i], sent[i]));
        }
        return Value(std::move(all));
    });

    view.expose("__begin", [this](const Value& arguments) {
        const auto index = indexOf(arguments[0]);
        change(EditorChange::Kind::begin, plugin.parameters()[index].id, 0.0);
        return Value();
    });

    view.expose("__set", [this](const Value& arguments) {
        const auto index = indexOf(arguments[0]);
        Access::set(plugin, index, arguments[1].numberOr(Access::value(plugin, index)));
        const auto value = Access::value(plugin, index);
        sent[index] = value;
        change(EditorChange::Kind::value, plugin.parameters()[index].id, value);
        return Value(Object{{"value", value}, {"text", textFor(plugin.parameters()[index], value)}});
    });

    view.expose("__end", [this](const Value& arguments) {
        const auto index = indexOf(arguments[0]);
        change(EditorChange::Kind::end, plugin.parameters()[index].id, 0.0);
        return Value();
    });

    view.expose("__text", [this](const Value& arguments) {
        const auto& parameter = plugin.parameters()[indexOf(arguments[0])];
        return Value(textFor(parameter, arguments[1].numberOr(parameter.defaultValue)));
    });

    if (!view.available())
        return;

    plugin.editorOpened(*this);
    timer = std::make_unique<ui::Timer>(33, [this] { tick(); });
}

EditorSession::~EditorSession()
{
    timer.reset();
    if (view.available())
    {
        plugin.editorClosed();
        ui::native::detach(view.nativeView());
    }
}

void EditorSession::expose(std::string name, std::function<Value(const Value& arguments)> function)
{
    view.expose(std::move(name), std::move(function));
}

void EditorSession::send(std::string_view event, const Value& data)
{
    view.send(event, data);
}

std::size_t EditorSession::indexOf(const Value& id) const
{
    const auto index = Access::index(plugin, static_cast<std::uint32_t>(id.intOr(-1)));
    if (!index)
        throw std::runtime_error("the plugin has no parameter " + std::to_string(id.intOr(-1)));
    return *index;
}

void EditorSession::change(EditorChange::Kind kind, std::uint32_t id, double value)
{
    changes.push({kind, id, value});
    if (told)
        told();
}

void EditorSession::tick()
{
    // A value the host changed, through automation or a preset, reaches the page here.
    for (std::size_t i = 0; i < sent.size(); ++i)
    {
        const auto value = Access::value(plugin, i);
        if (value != sent[i])
        {
            sent[i] = value;
            const auto& parameter = plugin.parameters()[i];
            view.send("__parameter",
                      Object{{"id", parameter.id}, {"value", value}, {"text", textFor(parameter, value)}});
        }
    }

    plugin.editorTick(*this);
}

}
