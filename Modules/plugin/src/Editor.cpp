#include "Editor.h"

#include "Access.h"
#include "TextValues.h"

#include <symbolengine/core/Paths.h>
#include <symbolengine/core/SettingsFile.h>
#include <symbolengine/core/System.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
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

// The timer's interval, and how many of its ticks a resizing rests for before its size is kept.
constexpr std::uint32_t tickMilliseconds = 33;
constexpr int restBeforeKeeping = 18;

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

ui::WebView::Options pageOptions(const Description& description, const Editor& editor)
{
    auto dataFolder = editor.dataFolder;
    if (dataFolder.empty())
    {
        std::error_code ignored;
        dataFolder = paths::systemFolder(paths::Folder::localAppData, ignored) / paths::fromUtf8(description.vendor) /
                     paths::fromUtf8(description.name) / "WebView";
    }

    return {.resources = [files = editor.files](std::string_view path) -> std::optional<ui::Resource> {
                if (path == "/plugin.js")
                    return ui::Resource{std::string(*symbolengine_plugin_page_files("/plugin.js")), "text/javascript"};
                if (!files)
                    return std::nullopt;
                const auto found = files(path);
                if (!found)
                    return std::nullopt;
                return ui::Resource{std::string(*found), ""};
            },
            .name = description.id,
            .dataFolder = dataFolder};
}

std::uint32_t whole(const Value& number)
{
    return static_cast<std::uint32_t>(std::clamp<std::int64_t>(number.intOr(0), 0, 100000));
}
}

EditorSession::EditorSession(Plugin& owner, const Description& about, Editor description, EditorChanges& queue,
                             Host link)
    : plugin(owner), editor(std::move(description)), changes(queue), host(std::move(link)),
      view(pageOptions(about, editor)), file(sizeFile(about))
{
    size = openingSize();
    kept = size;

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

    view.expose("__resize", [this](const Value& arguments) {
        if (!editor.resizable)
            throw std::runtime_error("this editor can't be resized");

        const auto [w, h] = heldToLimits(whole(arguments[0]), whole(arguments[1]));
        const bool agreed = (w == size.first && h == size.second) || (host.resize && host.resize(w, h));
        if (agreed)
            resizedTo(w, h);
        return Value(Object{{"width", size.first}, {"height", size.second}, {"agreed", agreed}});
    });

    if (!view.available())
        return;

    plugin.editorOpened(*this);
    timer = std::make_unique<ui::Timer>(tickMilliseconds, [this] { tick(); });
}

EditorSession::~EditorSession()
{
    timer.reset();
    if (view.available())
    {
        keepSize(true);
        plugin.editorClosed();
        ui::native::detach(view.nativeView());
    }
}

std::filesystem::path EditorSession::sizeFile(const Description& about) const
{
    if (!editor.sizeFile.empty())
        return editor.sizeFile;
    std::error_code ignored;
    return paths::systemFolder(paths::Folder::appData, ignored) / paths::fromUtf8(about.vendor) /
           paths::fromUtf8(about.name) / "Editor.json";
}

std::pair<std::uint32_t, std::uint32_t> EditorSession::heldToLimits(std::uint32_t w, std::uint32_t h) const noexcept
{
    if (!editor.resizable)
        return {editor.width, editor.height};
    w = std::max(w, editor.minimumWidth);
    h = std::max(h, editor.minimumHeight);
    if (editor.maximumWidth > 0)
        w = std::min(w, editor.maximumWidth);
    if (editor.maximumHeight > 0)
        h = std::min(h, editor.maximumHeight);
    return {w, h};
}

std::pair<std::uint32_t, std::uint32_t> EditorSession::openingSize() const
{
    if (!editor.resizable)
        return {editor.width, editor.height};

    // This instance's own size, or the size kept for new editors, or the size the plugin gives.
    auto [w, h] = Access::editorSize(plugin);
    if (w == 0 || h == 0)
    {
        w = editor.width;
        h = editor.height;
        std::error_code ignored;
        if (editor.remember && std::filesystem::exists(file, ignored))
        {
            const SettingsFile keptSize(file);
            w = whole(keptSize.get("width").isNumber() ? keptSize.get("width") : Value(w));
            h = whole(keptSize.get("height").isNumber() ? keptSize.get("height") : Value(h));
        }
    }

    // A size left from a larger screen is fitted to the one the editor opens on.
    const auto screen = ui::native::workAreaAtPointer();
    const auto pointsWide = static_cast<std::uint32_t>(screen.width / host.pixelsPerPoint);
    const auto pointsHigh = static_cast<std::uint32_t>(screen.height / host.pixelsPerPoint);
    if (pointsWide > 40 && pointsHigh > 80)
    {
        w = std::min(w, pointsWide - 40);
        h = std::min(h, pointsHigh - 80);
    }

    return heldToLimits(w, h);
}

void EditorSession::resizedTo(std::uint32_t w, std::uint32_t h)
{
    if (w == size.first && h == size.second)
        return;
    size = {w, h};
    restingTicks = 0;
    Access::keepEditorSize(plugin, w, h);
    plugin.editorResized(w, h);
}

void EditorSession::keepSize(bool now)
{
    if (!editor.resizable || !editor.remember || size == kept)
        return;
    if (!now && ++restingTicks < restBeforeKeeping)
        return;

    std::error_code ignored;
    SettingsFile keptSize(file);
    keptSize.set("width", size.first);
    keptSize.set("height", size.second);
    keptSize.save(ignored);
    kept = size;
}

void EditorSession::expose(std::string name, std::function<Value(const Value& arguments)> function)
{
    view.expose(std::move(name), std::move(function));
}

void EditorSession::send(std::string_view event, const Value& data)
{
    view.send(event, data);
}

void EditorSession::snapshot(const std::filesystem::path& png, std::function<void(bool)> done)
{
    view.snapshot(png, std::move(done));
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
    if (host.told)
        host.told();
}

// For checks run from a script: a second after the page is ready the script is run, and a second after that the
// picture is saved.
void EditorSession::snapshotWhenAsked()
{
    if (!view.pageReady())
        return;

    const int second = static_cast<int>(1000 / tickMilliseconds);
    if (++ticks != second && ticks != 2 * second)
        return;

    const auto png = system::environmentVariable("SYMBOLENGINE_EDITOR_SNAPSHOT");
    if (!png || png->empty())
        return;

    if (ticks == second)
    {
        if (const auto script = system::environmentVariable("SYMBOLENGINE_EDITOR_SCRIPT"))
            view.run(*script);
        return;
    }

    view.snapshot(paths::fromUtf8(*png), [path = *png](bool written) {
        std::printf("snapshot %s: %s\n", written ? "written" : "FAILED", path.c_str());
        std::fflush(stdout);
    });
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

    keepSize(false);
    snapshotWhenAsked();
    plugin.editorTick(*this);
}

}
