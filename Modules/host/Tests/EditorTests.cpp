#include <symbolengine/core/Files.h>
#include <symbolengine/host/FakePluginFormat.h>
#include <symbolengine/host/Plugin.h>
#include <symbolengine/host/Vst3Format.h>

#if SYMBOLENGINE_PLUGIN_WINDOWS
    #include <symbolengine/host/PluginWindow.h>
#endif

#include "TestMainThread.h"
#include "Vst3/TestIds.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

using namespace symbolengine;
using namespace std::chrono_literals;

#if SYMBOLENGINE_PLUGIN_WINDOWS
// From EditorTests_mac.mm and EditorTests_windows.cpp.
void turnSystemLoop();
ui::Size dragWindow(PluginWindow& window, ui::Size to);
void* keepWindow(void* window);
void letGoOfWindow(void* window);
bool holdsViews(void* window);
void beginWindowMove(void* window);
void endWindowMove(void* window);
void rescaleWindow(void* window, double scale);
#endif

namespace
{

// The view a program holds an editor in, recording the sizes the editor's format gives it.
struct Frame final : PluginEditorFrame
{
    std::vector<PluginEditorSize> placed;

    void place(PluginEditorSize size) override { placed.push_back(size); }
};

struct FakeRig
{
    TestMainThread main;
    FakePluginFormat fake{main.isCurrent(), main.post()};
    std::vector<std::string> wrong;
    PluginHost host{
        {main.post(), main.isCurrent(), 2000ms, [this](const std::string& what) { wrong.push_back(what); }}};
    std::vector<PluginChange> changes;

    std::shared_ptr<Plugin> made(const std::string& id, bool editor = true)
    {
        FakePluginFormat::Script script;
        script.editor = editor;
        script.editorSize = {420, 280};
        fake.script(id, script);
        auto plugin = host.make(fake, FakePluginFormat::describe(id));
        plugin->onChange = [this](PluginChange change) { changes.push_back(change); };
        REQUIRE(main.turnUntil([&] { return plugin->state() != Plugin::State::making; }));
        return plugin;
    }

    std::ptrdiff_t count(const std::string& id, const std::string& call) const
    {
        const auto calls = fake.calls(id);
        return std::count(calls.begin(), calls.end(), call);
    }

    void checkNoMisuse() const
    {
        const auto misuses = fake.misuses();
        INFO((misuses.empty() ? std::string() : misuses.front()));
        CHECK(misuses.empty());
        CHECK(wrong.empty());
    }
};

}

TEST_CASE("A plugin says whether it has an editor, asked of it once")
{
    FakeRig rig;
    auto with = rig.made("with");
    auto without = rig.made("without", false);

    CHECK(with->hasEditor());
    CHECK(with->hasEditor());
    CHECK(rig.count("with", "hasEditor") == 1);
    CHECK_FALSE(without->hasEditor());
    rig.checkNoMisuse();
}

TEST_CASE("A plugin's editor opens in the program's view, sized through its frame first, one at a time")
{
    FakeRig rig;
    auto plugin = rig.made("editor");
    Frame frame;
    PluginError error;

    auto editor = PluginEditor::open(plugin, &frame, frame, error);
    INFO(error.message);
    REQUIRE(editor != nullptr);
    CHECK(frame.placed == std::vector<PluginEditorSize>{{420, 280}});
    CHECK(editor->size() == PluginEditorSize{420, 280});
    CHECK(editor->resizable());

    // One at a time, and asking whether it has one opens none.
    PluginError second;
    CHECK(PluginEditor::open(plugin, &frame, frame, second) == nullptr);
    CHECK(second);
    CHECK(plugin->hasEditor());
    CHECK(rig.count("editor", "openEditor") == 1);
    CHECK(rig.count("editor", "hasEditor") == 0);

    // A drag asks it for the size it takes, and tells it the size it was given.
    CHECK(editor->constrain({500, 300}) == PluginEditorSize{500, 300});
    editor->resized({500, 300});
    CHECK(editor->size() == PluginEditorSize{500, 300});
    CHECK(rig.count("editor", "constrain") == 1);
    CHECK(rig.count("editor", "resized") == 1);

    editor.reset();
    CHECK(rig.count("editor", "closeEditor") == 1);
    editor = PluginEditor::open(plugin, &frame, frame, error);
    CHECK(editor != nullptr);
    editor.reset();
    rig.main.settle();
    rig.checkNoMisuse();
}

TEST_CASE("A plugin with no editor opens none, saying so")
{
    FakeRig rig;
    auto plugin = rig.made("plain", false);
    Frame frame;
    PluginError error;

    // Its format's own reason reaches the program.
    CHECK(PluginEditor::open(plugin, &frame, frame, error) == nullptr);
    CHECK(error.message == "The fake plugin has no editor.");
    CHECK(frame.placed.empty());
    rig.checkNoMisuse();
}

TEST_CASE(
    "An editor closed during a call into its plugin closes once the call returns, and its plugin goes a turn later")
{
    FakeRig rig;

    // Closed from inside a call into the plugin, as a click on a window's close button can come while a plugin's own
    // dialog turns the run loop.
    std::unique_ptr<PluginEditor> editor;
    bool closedInside = false;
    FakePluginFormat::Script script;
    script.editor = true;
    script.inside = [&](std::string_view call) {
        if (call != "setState" || editor == nullptr)
            return;

        editor.reset();
        closedInside = rig.count("modal", "closeEditor") > 0;
    };
    rig.fake.script("modal", script);
    auto plugin = rig.host.make(rig.fake, FakePluginFormat::describe("modal"));
    REQUIRE(rig.main.turnUntil([&] { return plugin->state() != Plugin::State::making; }));

    Frame frame;
    PluginError error;
    editor = PluginEditor::open(plugin, &frame, frame, error);
    REQUIRE(editor != nullptr);

    // The program lets go of the plugin, which its editor keeps.
    const std::weak_ptr<Plugin> watched = plugin;
    plugin.reset();
    rig.main.settle();
    REQUIRE(rig.fake.alive() == 1);

    CHECK(watched.lock()->loadState("anything", PluginStateUse::document));
    CHECK(editor == nullptr);
    CHECK_FALSE(closedInside);
    CHECK(rig.fake.alive() == 1);

    // Once the call has returned the editor closes, and the plugin goes a turn after that.
    rig.main.turn();
    CHECK(rig.count("modal", "closeEditor") == 1);
    CHECK(rig.fake.alive() == 1);
    rig.main.settle();
    CHECK(rig.fake.alive() == 0);
    rig.checkNoMisuse();
}

TEST_CASE("A plugin asking for its editor is told to the program")
{
    FakeRig rig;
    auto plugin = rig.made("asks");
    rig.fake.notify("asks", PluginNotice::editor);

    CHECK(rig.main.turnUntil(
        [&] { return std::find(rig.changes.begin(), rig.changes.end(), PluginChange::editor) != rig.changes.end(); }));
}

#if SYMBOLENGINE_PLUGIN_WINDOWS

TEST_CASE("A plugin window closed during a call into its plugin has its editor closed once the call returns, while the "
          "window is still whole")
{
    FakeRig rig;
    std::unique_ptr<PluginWindow> window;
    void* nativeWindow = nullptr;
    bool closedInside = false, wholeAtClose = false;

    FakePluginFormat::Script script;
    script.editor = true;
    script.inside = [&](std::string_view call) {
        if (call == "setState" && window != nullptr)
        {
            window.reset();
            closedInside = rig.count("modal", "closeEditor") > 0;
        }

        if (call == "closeEditor")
            wholeAtClose = holdsViews(nativeWindow);
    };
    rig.fake.script("modal", script);
    auto plugin = rig.host.make(rig.fake, FakePluginFormat::describe("modal"));
    REQUIRE(rig.main.turnUntil([&] { return plugin->state() != Plugin::State::making; }));

    PluginError error;
    window = PluginWindow::open(plugin, {"Busy"}, error);
    INFO(error.message);
    REQUIRE(window != nullptr);
    CHECK(window->editorSize() == PluginEditorSize{400, 300});
    nativeWindow = keepWindow(window->nativeHandle());

    CHECK(plugin->loadState("anything", PluginStateUse::document));
    CHECK(window == nullptr);
    CHECK_FALSE(closedInside);

    rig.main.settle();
    CHECK(rig.count("modal", "closeEditor") == 1);
    CHECK(wholeAtClose);
    letGoOfWindow(nativeWindow);
    rig.checkNoMisuse();
}

TEST_CASE("A plugin window tells its editor when its screen's scale changes, once the user's move is over")
{
    FakeRig rig;
    auto plugin = rig.made("scaled");
    PluginError error;
    auto window = PluginWindow::open(plugin, {"Scaled"}, error);
    INFO(error.message);
    REQUIRE(window != nullptr);

    beginWindowMove(window->nativeHandle());
    rescaleWindow(window->nativeHandle(), 1.5);
    #if defined(_WIN32)
    CHECK(rig.count("scaled", "rescaled") == 0);
    #endif
    endWindowMove(window->nativeHandle());
    CHECK(rig.count("scaled", "rescaled") == 1);
    window.reset();
    rig.main.settle();
    rig.checkNoMisuse();
}

namespace
{

const std::filesystem::path module = SYMBOLENGINE_TEST_VST3;
const std::filesystem::path misuseLog = SYMBOLENGINE_TEST_VST3_LOG;

std::string misuses()
{
    std::error_code missing;
    return files::read(misuseLog, missing);
}

struct Vst3Rig
{
    Vst3Rig()
    {
        std::error_code ignored;
        std::filesystem::remove(misuseLog, ignored);
    #if defined(_WIN32)
        _putenv_s("SYMBOLENGINE_TEST_VST3_LOG", misuseLog.string().c_str());
    #else
        setenv("SYMBOLENGINE_TEST_VST3_LOG", misuseLog.c_str(), 1);
    #endif
    }

    TestMainThread main;
    std::vector<std::string> wrong;
    Vst3Format format{{"SymbolEngine tests", main.post(), main.isCurrent()}};
    PluginHost host{
        {main.post(), main.isCurrent(), 2000ms, [this](const std::string& what) { wrong.push_back(what); }}};
    std::vector<PluginChange> changes;

    // The editor plugin, its view to behave as the mode says.
    std::shared_ptr<Plugin> made(testIds::EditorMode mode = testIds::wellBehaved, const char* id = testIds::editor)
    {
        auto plugin = host.make(format, Vst3Format::describe(module, id));
        plugin->onChange = [this](PluginChange change) { changes.push_back(change); };
        REQUIRE(main.turnUntil([&] { return plugin->state() != Plugin::State::making; }));
        INFO(plugin->problem().message);
        REQUIRE(plugin->state() == Plugin::State::inactive);
        if (std::string(id) == testIds::editor)
            REQUIRE(plugin->setParameter(testIds::viewMode, static_cast<double>(mode) / (testIds::editorModes - 1)));
        return plugin;
    }

    std::unique_ptr<PluginWindow> open(const std::shared_ptr<Plugin>& plugin)
    {
        PluginError error;
        auto window = PluginWindow::open(plugin, {"Editor test"}, error);
        INFO(error.message);
        REQUIRE(window != nullptr);
        return window;
    }

    // Turns the host's main thread and the system's run loop until the condition holds.
    template <typename Condition>
    bool turnUntil(Condition condition)
    {
        return main.turnUntil([&] {
            turnSystemLoop();
            return condition();
        });
    }

    void settle()
    {
        for (int i = 0; i < 5; ++i)
        {
            turnSystemLoop();
            main.settle();
        }
    }
};

// A number the editor plugin's query text gives, by name.
int seen(Plugin& plugin, const std::string& name)
{
    const auto text = plugin.parameterText(testIds::queryParameter, 0.0);
    const auto at = text.find(name + "=");
    return at == std::string::npos ? -1 : std::atoi(text.c_str() + at + name.size() + 1);
}

// A size the editor plugin's query text gives: the last it was told, as "size", or its own native view's, as "own".
PluginEditorSize seenSize(Plugin& plugin, const std::string& name = "size")
{
    const auto text = plugin.parameterText(testIds::queryParameter, 0.0);
    const auto at = text.find(" " + name + "=");
    if (at == std::string::npos)
        return {};
    const auto by = text.find('x', at);
    return {std::atoi(text.c_str() + at + name.size() + 2), std::atoi(text.c_str() + by + 1)};
}

// Has the editor plugin's view ask its host for a size, on the main thread or from a thread of its own.
void ask(Plugin& plugin, int width, int height, bool fromThread = false)
{
    REQUIRE(plugin.setParameter(testIds::askWidth, width / testIds::editorPixels));
    REQUIRE(plugin.setParameter(testIds::askHeight, height / testIds::editorPixels));
    REQUIRE(plugin.setParameter(testIds::askResize, fromThread ? 1.0 : 0.5));
}

ui::Size windowSize(PluginWindow& window)
{
    const auto bounds = window.bounds();
    return {bounds.width, bounds.height};
}

}

TEST_CASE("A VST3 plugin says whether it has an editor, asked once without one being opened")
{
    Vst3Rig rig;
    auto editor = rig.made();
    auto gain = rig.made(testIds::wellBehaved, testIds::gain);

    CHECK(editor->hasEditor());
    CHECK(editor->hasEditor());
    CHECK(seen(*editor, "made") == 1);
    CHECK(seen(*editor, "live") == 0);
    CHECK(seen(*editor, "attached") == 0);
    CHECK_FALSE(gain->hasEditor());
    CHECK(misuses().empty());
}

TEST_CASE("A VST3 editor opens in its window at its own size, in its format's order, and closes in order")
{
    Vst3Rig rig;
    auto plugin = rig.made();
    auto window = rig.open(plugin);

    CHECK(window->editorSize() == PluginEditorSize{600, 400});
    CHECK(windowSize(*window) == ui::Size{600, 400});
    CHECK(window->isResizable());
    CHECK(seen(*plugin, "attached") == 1);
    CHECK(seen(*plugin, "live") == 1);

    // Its screen's scale on Windows, where it is sized in pixels, before its size is first asked for and again once
    // it is in its window; never on a Mac.
    #if defined(_WIN32)
    CHECK(seen(*plugin, "scales") == 2);
    CHECK(plugin->parameterText(testIds::scaleQuery, 0.0) == "scale=100 first=1");
    #else
    CHECK(seen(*plugin, "scales") == 0);
    #endif

    window.reset();
    rig.settle();
    CHECK(seen(*plugin, "removed") == 1);
    CHECK(seen(*plugin, "live") == 0);
    CHECK(misuses().empty());
    CHECK(rig.wrong.empty());
}

TEST_CASE("A VST3 editor opened and closed 200 times never has two views, and a plugin let go of with its window open "
          "has its editor closed first")
{
    Vst3Rig rig;
    auto plugin = rig.made();

    for (int i = 0; i < 200; ++i)
    {
        auto window = rig.open(plugin);
        CHECK(seen(*plugin, "live") == 1);
        window.reset();
        rig.main.settle();
    }

    rig.settle();
    CHECK(seen(*plugin, "live") == 0);
    CHECK(seen(*plugin, "removed") == 200);

    // The program lets go of the plugin while its window is open, then closes the window: the editor closes before
    // the plugin is taken down, as the plugin checks.
    auto window = rig.open(plugin);
    plugin.reset();
    rig.settle();
    window.reset();
    rig.settle();
    CHECK(misuses().empty());
}

TEST_CASE("A second editor of a VST3 plugin is refused while one is open")
{
    Vst3Rig rig;
    auto plugin = rig.made();
    auto window = rig.open(plugin);

    PluginError error;
    CHECK(PluginWindow::open(plugin, {"Second"}, error) == nullptr);
    CHECK(error);
    CHECK(seen(*plugin, "made") == 1);
    CHECK(misuses().empty());
}

TEST_CASE("A VST3 editor asking to be resized is resized on the main thread, its window following, and told once")
{
    Vst3Rig rig;
    auto plugin = rig.made();
    auto window = rig.open(plugin);
    const auto before = seen(*plugin, "sized");

    ask(*plugin, 700, 500);
    CHECK(window->editorSize() == PluginEditorSize{700, 500});
    CHECK(windowSize(*window) == ui::Size{700, 500});
    CHECK(seen(*plugin, "sized") == before + 1);
    CHECK(seenSize(*plugin) == PluginEditorSize{700, 500});

    // From a thread of its own: handled on the main thread.
    ask(*plugin, 800, 300, true);
    CHECK(rig.turnUntil([&] { return window->editorSize() == PluginEditorSize{800, 300}; }));
    CHECK(windowSize(*window) == ui::Size{800, 300});
    CHECK(seen(*plugin, "sized") == before + 2);
    CHECK(seen(*plugin, "offMain") == 0);
    CHECK(misuses().empty());
}

TEST_CASE("VST3 editors that ask again from inside their own resize never loop, and none is made too small")
{
    Vst3Rig rig;

    SECTION("asking for the same size again")
    {
        auto plugin = rig.made(testIds::asksSameSize);
        auto window = rig.open(plugin);
        const auto before = seen(*plugin, "sized");
        ask(*plugin, 700, 500);
        CHECK(window->editorSize() == PluginEditorSize{700, 500});
        CHECK(seen(*plugin, "sized") == before + 1);
    }

    SECTION("correcting the size to keep its proportions")
    {
        auto plugin = rig.made(testIds::keepsAspect);
        auto window = rig.open(plugin);
        const auto before = seen(*plugin, "sized");
        ask(*plugin, 800, 300);
        CHECK(window->editorSize() == PluginEditorSize{800, 400});
        CHECK(windowSize(*window) == ui::Size{800, 400});
        CHECK(seen(*plugin, "sized") - before <= 3);
    }

    SECTION("asking to grow from inside every resize")
    {
        auto plugin = rig.made(testIds::keepsGrowing);
        auto window = rig.open(plugin);
        const auto before = seen(*plugin, "sized");
        ask(*plugin, 700, 500);
        CHECK(seen(*plugin, "sized") - before <= 4);
        CHECK(window->editorSize().width <= 704);
        rig.settle();
        CHECK(seen(*plugin, "sized") - before <= 4);
    }

    SECTION("saying it can't be resized, yet asking to be, and leaving its own view's size to the host")
    {
        auto plugin = rig.made(testIds::fixedButAsks);
        auto window = rig.open(plugin);
        CHECK_FALSE(window->isResizable());
        ask(*plugin, 700, 500);
        CHECK(window->editorSize() == PluginEditorSize{700, 500});
        CHECK(windowSize(*window) == ui::Size{700, 500});
    #if defined(__APPLE__)
        CHECK(seenSize(*plugin, "own") == PluginEditorSize{700, 500});
    #endif
    }

    SECTION("asking for one pixel")
    {
        auto plugin = rig.made();
        auto window = rig.open(plugin);
        const auto before = seen(*plugin, "sized");
        ask(*plugin, 1, 1);
        CHECK(seen(*plugin, "sized") == before);
        CHECK(window->editorSize().width >= 100);
        CHECK(window->editorSize().height >= 50);
        CHECK(windowSize(*window).width >= 100);
        CHECK(windowSize(*window).height >= 50);
    }

    rig.settle();
    CHECK(misuses().empty());
}

TEST_CASE("Dragging a VST3 editor's window asks it for the nearest size it takes, and tells it once per step")
{
    Vst3Rig rig;

    SECTION("a resizable editor held to its smallest size")
    {
        auto plugin = rig.made();
        auto window = rig.open(plugin);
        const auto before = seen(*plugin, "sized");
        CHECK(dragWindow(*window, {150, 80}) == ui::Size{200, 100});
        CHECK(window->editorSize() == PluginEditorSize{200, 100});
        CHECK(seenSize(*plugin) == PluginEditorSize{200, 100});
        CHECK(seen(*plugin, "sized") == before + 1);
    }

    SECTION("an editor that answers every size with one a pixel larger")
    {
        auto plugin = rig.made(testIds::onePixelMore);
        auto window = rig.open(plugin);
        const auto before = seen(*plugin, "sized");

        for (int step = 1; step <= 10; ++step)
        {
            const ui::Size to{600 + 10 * step, 400 + 10 * step};
            CHECK(dragWindow(*window, to) == ui::Size{to.width + 1, to.height + 1});
            CHECK(seen(*plugin, "sized") == before + step);
        }
    }

    SECTION("a fixed editor is never asked")
    {
        auto plugin = rig.made(testIds::fixedSize);
        auto window = rig.open(plugin);
        CHECK_FALSE(window->isResizable());
        CHECK(dragWindow(*window, {700, 500}) == ui::Size{600, 400});
        CHECK(seen(*plugin, "constraints") == 0);
        CHECK(window->editorSize() == PluginEditorSize{600, 400});
    }

    rig.settle();
    CHECK(misuses().empty());
}

TEST_CASE(
    "A VST3 editor's view stays in one view of its window's, which is never flipped, hidden or swapped, its other "
    "views are never resized, and it is settled once after opening")
{
    Vst3Rig rig;
    auto plugin = rig.made();
    auto window = rig.open(plugin);
    rig.settle();

    ask(*plugin, 700, 500);
    dragWindow(*window, {800, 600});
    window.reset();
    rig.settle();

    CHECK(seen(*plugin, "removed") == 1);
    CHECK(seen(*plugin, "offscreenMoved") == 0);
    #if defined(__APPLE__)
    CHECK(seen(*plugin, "nudges") == 1);
    #endif
    CHECK(misuses().empty());
}

    #if defined(_WIN32)
TEST_CASE("A VST3 editor on Windows takes the size it gives at its screen's new scale once a move there is over")
{
    Vst3Rig rig;
    auto plugin = rig.made();
    auto window = rig.open(plugin);
    REQUIRE(seen(*plugin, "scales") == 2);

    // Dragged to a screen at 150%: nothing changes until the move is over, then once.
    beginWindowMove(window->nativeHandle());
    rescaleWindow(window->nativeHandle(), 1.5);
    CHECK(seen(*plugin, "scales") == 2);
    CHECK(window->editorSize() == PluginEditorSize{600, 400});
    endWindowMove(window->nativeHandle());

    CHECK(seen(*plugin, "scales") == 3);
    CHECK(plugin->parameterText(testIds::scaleQuery, 0.0) == "scale=150 first=1");
    CHECK(window->editorSize() == PluginEditorSize{900, 600});
    CHECK(windowSize(*window) == ui::Size{900, 600});
    CHECK(seenSize(*plugin) == PluginEditorSize{900, 600});
    window.reset();
    rig.settle();
    CHECK(misuses().empty());
}

TEST_CASE("A VST3 editor on Windows keeps its parent through a thousand moves and resizes of its window")
{
    Vst3Rig rig;
    auto plugin = rig.made();
    auto window = rig.open(plugin);
    const auto start = window->bounds();

    for (int i = 0; i < 1000; ++i)
        if (i % 2 == 0)
            window->moveTo(start.x + i % 40, start.y + i % 30);
        else
            dragWindow(*window, {600 + i % 50, 400 + i % 30});

    rig.settle();
    window.reset();
    rig.settle();
    CHECK(seen(*plugin, "removed") == 1);
    CHECK(misuses().empty());
}
    #endif

TEST_CASE("A VST3 plugin asking for its editor is told to the program")
{
    Vst3Rig rig;
    auto plugin = rig.made();
    REQUIRE(plugin->setParameter(testIds::requestEditor, 1.0));

    CHECK(rig.turnUntil(
        [&] { return std::find(rig.changes.begin(), rig.changes.end(), PluginChange::editor) != rig.changes.end(); }));
}

#endif
