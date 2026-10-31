#include <symbolengine/core/Files.h>
#include <symbolengine/host/FakePluginFormat.h>
#include <symbolengine/host/Plugin.h>
#include <symbolengine/host/Vst3Format.h>

#if SYMBOLENGINE_PLUGIN_WINDOWS
    #include <symbolengine/host/PluginWindow.h>
    #include <symbolengine/host/PluginWindows.h>
    #include <symbolengine/ui/MessageLoop.h>
#endif

#include "TestMainThread.h"
#include "Vst3/TestIds.h"

#if SYMBOLENGINE_PLUGIN_WINDOWS && defined(__APPLE__)
    #include <symbolengine/host/AudioUnitFormat.h>

    #include "AudioUnits/Registry.h"
#endif

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <optional>
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
bool editorShown(PluginWindow& window);
bool floatsAbove(void* window);
void* showOtherWindow();
void closeOtherWindow(void* window);
void pressKey(PluginWindow& window, char32_t key, bool repeat = false, bool command = false);
void* typeIn(PluginWindow& window);
void stopTyping(void* field);
#endif
#if SYMBOLENGINE_PLUGIN_WINDOWS && defined(__APPLE__)
bool handUp(PluginWindow& window, char32_t key);
void* showMenuWindow();
int viewsInEditor(void* window);
ui::Size ownViewSize(void* window);
bool ownViewAtTopLeft(void* window);
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
          "window is still whole",
          "[windows]")
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

TEST_CASE("A plugin window tells its editor when its screen's scale changes, once the user's move is over", "[windows]")
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

TEST_CASE("Plugin windows float above the program's while it is in front, and step back while another window of its "
          "process shows",
          "[windows]")
{
    FakeRig rig;
    ui::Window program({.title = "Program"});
    bool front = true;
    void* tool = nullptr;
    PluginWindows group({&program,
                         [&](void* window) { return window == tool; },
                         {U' '},
                         nullptr,
                         [&] { return front; },
                         100ms,
                         nullptr});
    auto first = rig.made("first");
    auto second = rig.made("second");
    PluginError error;
    auto one = PluginWindow::open(first, {"One", 0, &group}, error);
    auto two = PluginWindow::open(second, {"Two", 0, &group}, error);
    REQUIRE(one != nullptr);
    REQUIRE(two != nullptr);
    const auto turnUntil = [&](auto condition) {
        return rig.main.turnUntil([&] {
            turnSystemLoop();
            return condition();
        });
    };

    CHECK(group.floating());
    CHECK(floatsAbove(one->nativeHandle()));
    CHECK(floatsAbove(two->nativeHandle()));

    // Behind other programs' windows while the program isn't in front.
    front = false;
    group.arrange();
    CHECK_FALSE(group.floating());
    CHECK_FALSE(floatsAbove(one->nativeHandle()));
    front = true;
    group.arrange();
    CHECK(floatsAbove(one->nativeHandle()));

    // Another window of the process showing, as a question, a file chooser or a plugin's own window: they step back
    // until it has gone, which the group sees itself.
    auto other = showOtherWindow();
    CHECK(turnUntil([&] { return !group.floating(); }));
    CHECK_FALSE(floatsAbove(one->nativeHandle()));
    CHECK_FALSE(floatsAbove(two->nativeHandle()));
    closeOtherWindow(other);
    CHECK(turnUntil([&] { return group.floating(); }));
    CHECK(floatsAbove(two->nativeHandle()));

    // A window of the program's own leaves them floating, as does a menu or a tooltip, which lies above them.
    tool = showOtherWindow();
    group.arrange();
    CHECK(group.floating());
    closeOtherWindow(tool);
    tool = nullptr;
    #if defined(__APPLE__)
    auto menu = showMenuWindow();
    group.arrange();
    CHECK(group.floating());
    closeOtherWindow(menu);
    #endif

    one.reset();
    two.reset();
    rig.main.settle();
    rig.checkNoMisuse();
}

TEST_CASE("The program's keys reach it from a plugin window whatever the plugin does with keys, once a press and after "
          "the key's event, unless text is being typed there",
          "[windows]")
{
    FakeRig rig;
    ui::Window program({.title = "Program"});
    std::vector<char32_t> taken;
    PluginWindows group(
        {&program, nullptr, {U' '}, [&](char32_t key) { taken.push_back(key); }, [] { return true; }, 100ms, nullptr});
    auto plugin = rig.made("keys");
    PluginError error;
    auto window = PluginWindow::open(plugin, {"Keys", 0, &group}, error);
    REQUIRE(window != nullptr);
    const auto settle = [&] {
        for (int i = 0; i < 5; ++i)
        {
            turnSystemLoop();
            rig.main.settle();
        }
    };

    pressKey(*window, U' ');
    CHECK(taken.empty());
    settle();
    CHECK(taken == std::vector<char32_t>{U' '});

    // Held down, it repeats: taken once. Another key isn't the program's, nor is it with a modifier key, which a Mac's
    // test can press as Windows' can't, as Windows reads modifier keys from the keyboard itself.
    pressKey(*window, U' ', true);
    pressKey(*window, U'a');
    #if defined(__APPLE__)
    pressKey(*window, U' ', false, true);
    #endif
    settle();
    CHECK(taken.size() == 1);

    // Typed in a text field there, it is the field's.
    auto field = typeIn(*window);
    pressKey(*window, U' ');
    settle();
    CHECK(taken.size() == 1);
    stopTyping(field);

    pressKey(*window, U' ');
    settle();
    CHECK(taken.size() == 2);

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

TEST_CASE("A VST3 plugin says whether it has an editor, asked once without one being opened", "[windows]")
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

TEST_CASE("A VST3 editor opens in its window at its own size, in its format's order, and closes in order", "[windows]")
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
          "has its editor closed first",
          "[windows]")
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

TEST_CASE("A second editor of a VST3 plugin is refused while one is open", "[windows]")
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

TEST_CASE("A VST3 editor asking to be resized is resized on the main thread, its window following, and told once",
          "[windows]")
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

TEST_CASE("VST3 editors that ask again from inside their own resize never loop, and none is made too small",
          "[windows]")
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

TEST_CASE("Dragging a VST3 editor's window asks it for the nearest size it takes, and tells it once per step",
          "[windows]")
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

namespace
{

// The value of a script run in a window's page of parameters, once it gives one.
template <typename Turn>
Value inPage(PluginWindow& window, const std::string& script, Turn turnUntil)
{
    auto* page = window.parameterPage();
    REQUIRE(page != nullptr);
    std::optional<Value> result;
    page->run(script, [&](const Value& value, const std::string& error) {
        result = error.empty() ? value : Value("error: " + error);
    });
    REQUIRE(turnUntil([&] { return result.has_value(); }));
    return *result;
}

// The page's rows, once it has drawn them: each parameter's id, name, text, control and whether it is disabled.
template <typename Turn>
Value pageRows(PluginWindow& window, Turn turnUntil)
{
    const std::string script = R"(Array.from(document.querySelectorAll("[data-parameter]")).map(row => {
        const control = row.querySelector("input, select");
        return {id: Number(row.dataset.parameter), name: row.querySelector(".name").textContent,
                text: row.querySelector(".text").textContent, control: control.type, disabled: control.disabled};
    }))";
    Value rows;
    for (int i = 0; i < 200 && rows.size() == 0; ++i)
        rows = inPage(window, script, turnUntil);
    return rows;
}

}

TEST_CASE("A plugin with no editor gets a page of its parameters in its window, each with the plugin's own text, the "
          "hidden ones and its bypass left out",
          "[windows]")
{
    ui::messageLoop::initialize();
    Vst3Rig rig;
    auto plugin = rig.made(testIds::wellBehaved, testIds::gain);
    PluginError error;
    auto window = PluginWindow::open(plugin, {"Gain"}, error);
    INFO(error.message);
    REQUIRE(window != nullptr);
    CHECK(window->editor() == nullptr);
    CHECK(window->editorProblem().message == "It has no editor.");

    // Listed in the plugin's order: gain, meter, program, echo and rounded echo; never the hidden ones or the bypass.
    const auto rows = pageRows(*window, [&](auto condition) { return rig.turnUntil(condition); });
    std::vector<std::int64_t> ids;
    for (std::size_t i = 0; i < rows.size(); ++i)
        ids.push_back(rows[i]["id"].intOr(-1));
    CHECK(ids == std::vector<std::int64_t>{testIds::gainParameter, testIds::meterParameter, testIds::programParameter,
                                           testIds::echoParameter, testIds::roundedEchoParameter});
    CHECK(rows[0]["name"].stringOr("") == "Gain");
    CHECK(rows[0]["text"].stringOr("") == plugin->parameterText(testIds::gainParameter, 0.5));
    CHECK(rows[0]["control"].stringOr("") == "range");
    CHECK(rows[1]["disabled"] == Value(true));
    CHECK(rows[2]["control"].stringOr("") == "select-one");

    window.reset();
    rig.settle();
    CHECK(misuses().empty());
}

TEST_CASE("A move on the page of parameters reaches the plugin and the program as its own editor's would, in a "
          "gesture, and the page follows values changed elsewhere",
          "[windows]")
{
    ui::messageLoop::initialize();
    Vst3Rig rig;
    auto plugin = rig.made(testIds::wellBehaved, testIds::gain);
    std::vector<std::pair<PluginEdit, bool>> edits;
    plugin->onEdit = [&](const PluginEdit& edit, bool byUser) { edits.emplace_back(edit, byUser); };
    PluginError error;
    auto window = PluginWindow::open(plugin, {"Gain"}, error);
    REQUIRE(window != nullptr);
    const auto turnUntil = [&](auto condition) { return rig.turnUntil(condition); };
    REQUIRE(pageRows(*window, turnUntil).size() > 0);

    inPage(*window, R"((() => {
        const slider = document.querySelector('[data-parameter="100"] input');
        slider.dispatchEvent(new PointerEvent("pointerdown"));
        slider.value = 0.25;
        slider.dispatchEvent(new Event("input"));
        slider.dispatchEvent(new Event("change"));
        return true;
    })())",
           turnUntil);
    CHECK(rig.turnUntil([&] { return plugin->parameter(testIds::gainParameter) == 0.25 && edits.size() >= 3; }));
    REQUIRE(edits.size() == 3);
    CHECK(edits[0].first == PluginEdit{PluginEdit::Kind::begin, testIds::gainParameter, 0.0});
    CHECK(edits[1].first == PluginEdit{PluginEdit::Kind::value, testIds::gainParameter, 0.25});
    CHECK(edits[2].first == PluginEdit{PluginEdit::Kind::end, testIds::gainParameter, 0.0});
    CHECK(std::all_of(edits.begin(), edits.end(), [](const auto& edit) { return edit.second; }));

    // A read-only one, as a meter, is never set from the page, whatever the page asks.
    inPage(*window, R"(symbolengine.call("set", 3000000, 0.9))", turnUntil);
    rig.settle();
    CHECK(plugin->parameter(testIds::meterParameter) == 0.0);
    CHECK(edits.size() == 3);

    // Changed by the program: the page shows the plugin's text for it.
    REQUIRE(plugin->setParameter(testIds::gainParameter, 0.75));
    const auto wanted = plugin->parameterText(testIds::gainParameter, 0.75);
    std::string shown;
    for (int i = 0; i < 200 && shown != wanted; ++i)
        shown = inPage(*window, R"(document.querySelector('[data-parameter="100"] .text').textContent)", turnUntil)
                    .stringOr("");
    CHECK(shown == wanted);
    CHECK(inPage(*window, R"(Number(document.querySelector('[data-parameter="100"] input').value))", turnUntil)
              .numberOr(0.0) == 0.75);

    window.reset();
    rig.settle();
    CHECK(misuses().empty());
}

TEST_CASE("A program's own page of parameters is served in place of SymbolEngine's, reaching the plugin through the "
          "same functions",
          "[windows]")
{
    ui::messageLoop::initialize();
    Vst3Rig rig;
    auto plugin = rig.made(testIds::wellBehaved, testIds::gain);
    PluginWindow::Options options;
    options.title = "Own page";
    options.page = [](std::string_view path) -> std::optional<ui::Resource> {
        if (path != "/index.html")
            return std::nullopt;
        return ui::Resource{R"(<!doctype html><script src="symbolengine.js"></script><script>
            symbolengine.call("parameters").then(list => symbolengine.call("report", list.length, list[0].name));
            </script>)",
                            "text/html"};
    };
    PluginError error;
    auto window = PluginWindow::open(plugin, options, error);
    REQUIRE(window != nullptr);
    REQUIRE(window->parameterPage() != nullptr);
    std::optional<std::pair<std::int64_t, std::string>> reported;
    window->parameterPage()->expose("report", [&](const Value& arguments) {
        reported = std::make_pair(arguments[0].intOr(-1), arguments[1].stringOr(""));
        return Value();
    });

    CHECK(rig.turnUntil([&] { return reported.has_value(); }));
    REQUIRE(reported);
    CHECK(reported->first == 5);
    CHECK(reported->second == "Gain");
}

    #if defined(_WIN32)
TEST_CASE("A VST3 editor on Windows takes the size it gives at its screen's new scale once a move there is over",
          "[windows]")
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

TEST_CASE("Keys other than the program's are offered to a VST3 editor on Windows first, and sent to its window only "
          "when it doesn't take them, never twice",
          "[windows]")
{
    Vst3Rig rig;
    auto plugin = rig.made();
    PluginWindows group({nullptr, nullptr, {U' '}, nullptr, [] { return true; }, 100ms, nullptr});
    PluginError error;
    auto window = PluginWindow::open(plugin, {"Keys", 0, &group}, error);
    REQUIRE(window != nullptr);
    const auto before = plugin->parameterText(testIds::keysQuery, 0.0);
    REQUIRE(before == "offered=0 taken=0 sent=0");

    // Taken by the editor: never sent to its window. Not taken: sent once.
    pressKey(*window, U'a');
    pressKey(*window, U'b');
    rig.settle();
    CHECK(plugin->parameterText(testIds::keysQuery, 0.0) == "offered=2 taken=1 sent=1");
    window.reset();
    rig.settle();
    CHECK(misuses().empty());
}

TEST_CASE("A plugin that crashes on Windows has its editor hidden at once, so the system sends its view nothing more",
          "[windows]")
{
    Vst3Rig rig;
    auto plugin = rig.made();
    auto window = rig.open(plugin);
    REQUIRE(editorShown(*window));

    plugin->setParameter(testIds::crashOnMain, 1.0);
    CHECK(plugin->hasCrashed());
    CHECK_FALSE(editorShown(*window));
    window.reset();
    rig.settle();
}

TEST_CASE("A VST3 editor on Windows keeps its parent through a thousand moves and resizes of its window", "[windows]")
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

TEST_CASE("A main thread held up while plugin windows are open is said, with the plugins whose windows were open",
          "[windows]")
{
    Vst3Rig rig;
    auto plugin = rig.made();
    std::vector<std::pair<std::chrono::milliseconds, std::vector<std::shared_ptr<Plugin>>>> stalls;
    PluginWindows::Options options;
    options.inFront = [] { return true; };
    options.onStall = [&](std::chrono::milliseconds stall, const std::vector<std::shared_ptr<Plugin>>& plugins) {
        stalls.emplace_back(stall, plugins);
    };
    PluginWindows group(options);
    PluginError error;
    auto window = PluginWindow::open(plugin, {"Held up", 0, &group}, error);
    REQUIRE(window != nullptr);

    // A while of ordinary beats first, none of them said, then its view holds the main thread up for 150 ms, as a
    // view that redraws for too long does.
    const auto until = std::chrono::steady_clock::now() + 200ms;
    rig.turnUntil([&] { return std::chrono::steady_clock::now() > until; });
    REQUIRE(plugin->setParameter(testIds::holdMainThread, 0.15));
    CHECK(rig.turnUntil([&] {
        return std::any_of(stalls.begin(), stalls.end(), [](const auto& stall) { return stall.first >= 120ms; });
    }));
    REQUIRE_FALSE(stalls.empty());
    for (const auto& [stall, plugins] : stalls)
    {
        CHECK(stall > 100ms);
        CHECK(plugins == std::vector<std::shared_ptr<Plugin>>{plugin});
    }
    window.reset();
    rig.settle();
}

TEST_CASE("A VST3 plugin asking for its editor is told to the program", "[windows]")
{
    Vst3Rig rig;
    auto plugin = rig.made();
    REQUIRE(plugin->setParameter(testIds::requestEditor, 1.0));

    CHECK(rig.turnUntil(
        [&] { return std::find(rig.changes.begin(), rig.changes.end(), PluginChange::editor) != rig.changes.end(); }));
}

#endif

#if SYMBOLENGINE_PLUGIN_WINDOWS && defined(__APPLE__)

namespace
{

const std::filesystem::path units = SYMBOLENGINE_TEST_AUDIO_UNITS;
const std::filesystem::path twinUnits = SYMBOLENGINE_TEST_TWIN_AUDIO_UNITS;
const std::filesystem::path unitLog = SYMBOLENGINE_TEST_AU_LOG;

std::string unitMisuses()
{
    std::error_code missing;
    return files::read(unitLog, missing);
}

OSType code(const char (&four)[5])
{
    return static_cast<OSType>(static_cast<unsigned char>(four[0])) << 24 |
           static_cast<OSType>(static_cast<unsigned char>(four[1])) << 16 |
           static_cast<OSType>(static_cast<unsigned char>(four[2])) << 8 |
           static_cast<OSType>(static_cast<unsigned char>(four[3]));
}

// The test component's units, and the twin's, registered with this process in that order, as the twin's view class
// is to collide with the first's.
struct UnitRig
{
    UnitRig()
    {
        REQUIRE(testAudioUnits::registerAll(units));
        REQUIRE(testAudioUnits::registerAll(twinUnits));
        std::error_code ignored;
        std::filesystem::remove(unitLog, ignored);
        setenv("SYMBOLENGINE_TEST_AU_LOG", unitLog.c_str(), 1);
    }

    TestMainThread main;
    std::vector<std::string> wrong;
    AudioUnitFormat format{{main.post(), main.isCurrent(), nullptr}};
    PluginHost host{
        {main.post(), main.isCurrent(), 2000ms, [this](const std::string& what) { wrong.push_back(what); }}};

    // A unit, made and, unless asked not to be, prepared, which initializes it.
    std::shared_ptr<Plugin> made(const char (&subtype)[5], bool prepared = true)
    {
        PluginError error;
        const auto identity = PluginIdentity::audioUnit(code("aufx"), code(subtype), code("SyTs"));
        PluginDescription description;
        for (const auto& component : {units, twinUnits})
            for (const auto& plugin : format.pluginsIn(component, error))
                if (plugin.identity == identity)
                    description = plugin;
        REQUIRE(description.identity == identity);
        auto plugin = host.make(format, description);
        REQUIRE(main.turnUntil([&] { return plugin->state() != Plugin::State::making; }));
        INFO(plugin->problem().message);
        REQUIRE(plugin->state() == Plugin::State::inactive);
        if (prepared)
            REQUIRE(plugin->prepare({48000.0, 512, false}));
        return plugin;
    }

    template <typename Condition>
    bool turnUntil(Condition condition, std::chrono::milliseconds limit = 5s)
    {
        return main.turnUntil(
            [&] {
                turnSystemLoop();
                return condition();
            },
            limit);
    }

    // Turns both for a while, as views that change on timers are given time to.
    void turnFor(std::chrono::milliseconds time)
    {
        const auto until = std::chrono::steady_clock::now() + time;
        turnUntil([&] { return std::chrono::steady_clock::now() > until; }, time + 1s);
    }
};

std::unique_ptr<PluginWindow> openWindow(const std::shared_ptr<Plugin>& plugin, PluginError& error)
{
    PluginWindow::Options options;
    options.title = "Audio Unit editor test";
    options.parameterPage = false;
    return PluginWindow::open(plugin, options, error);
}

}

TEST_CASE("Keys a plugin's view hands up to its window's view are taken there quietly, the program's for the program",
          "[windows]")
{
    FakeRig rig;
    ui::Window program({.title = "Program"});
    std::vector<char32_t> taken;
    PluginWindows group(
        {&program, nullptr, {U' '}, [&](char32_t key) { taken.push_back(key); }, [] { return true; }, 100ms, nullptr});
    auto plugin = rig.made("handsUp");
    PluginError error;
    auto window = PluginWindow::open(plugin, {"Hands up", 0, &group}, error);
    REQUIRE(window != nullptr);

    // Taken by the window's view, rather than passed on to be refused with a beep.
    CHECK(handUp(*window, U'a'));
    CHECK(handUp(*window, U' '));
    for (int i = 0; i < 5; ++i)
    {
        turnSystemLoop();
        rig.main.settle();
    }
    CHECK(taken == std::vector<char32_t>{U' '});
}

TEST_CASE("An Audio Unit says whether it has a view, asked without one being made", "[windows]")
{
    UnitRig rig;
    CHECK(rig.made("VFix", false)->hasEditor());
    CHECK_FALSE(rig.made("Pass", false)->hasEditor());
    CHECK(unitMisuses().empty());
}

TEST_CASE("An Audio Unit's view opens only on an initialized unit, at its own size, and is taken out before the unit "
          "is uninitialized and put back once it is initialized again",
          "[windows]")
{
    UnitRig rig;
    auto plugin = rig.made("VFix", false);
    PluginError error;
    CHECK(openWindow(plugin, error) == nullptr);
    CHECK(error);

    REQUIRE(plugin->prepare({48000.0, 512, false}));
    auto window = openWindow(plugin, error);
    INFO(error.message);
    REQUIRE(window != nullptr);
    CHECK(window->editorSize() == PluginEditorSize{500, 300});
    CHECK(windowSize(*window) == ui::Size{500, 300});
    CHECK_FALSE(window->isResizable());
    CHECK(viewsInEditor(window->nativeHandle()) == 1);
    CHECK(ownViewAtTopLeft(window->nativeHandle()));

    // Prepared again at another rate, and let go of: its view out while it is uninitialized, and back after.
    REQUIRE(plugin->prepare({44100.0, 256, false}));
    CHECK(viewsInEditor(window->nativeHandle()) == 1);
    plugin->release();
    CHECK(viewsInEditor(window->nativeHandle()) == 0);
    REQUIRE(plugin->prepare({48000.0, 512, false}));
    CHECK(viewsInEditor(window->nativeHandle()) == 1);

    window.reset();
    rig.turnFor(50ms);
    CHECK(unitMisuses().empty());
    CHECK(rig.wrong.empty());
}

TEST_CASE("An Audio Unit's view that sizes itself is followed by its window, never fed back, a size of none taken as "
          "not yet known and an origin it moves put back",
          "[windows]")
{
    UnitRig rig;
    PluginError error;

    SECTION("growing on a timer")
    {
        auto plugin = rig.made("VGro");
        auto window = openWindow(plugin, error);
        REQUIRE(window != nullptr);
        CHECK(rig.turnUntil([&] { return windowSize(*window) == ui::Size{550, 350}; }));
        rig.turnFor(300ms);
        CHECK(windowSize(*window) == ui::Size{550, 350});
        CHECK(window->editorSize() == PluginEditorSize{550, 350});
        CHECK(ownViewSize(window->nativeHandle()) == ui::Size{550, 350});
    }

    SECTION("saying its size only once shown")
    {
        auto plugin = rig.made("VZer");
        auto window = openWindow(plugin, error);
        REQUIRE(window != nullptr);
        CHECK(windowSize(*window).width >= 100);
        CHECK(rig.turnUntil([&] { return windowSize(*window) == ui::Size{600, 400}; }));
        CHECK(ownViewAtTopLeft(window->nativeHandle()));
    }

    SECTION("moving its own origin")
    {
        auto plugin = rig.made("VOri");
        auto window = openWindow(plugin, error);
        REQUIRE(window != nullptr);
        CHECK(rig.turnUntil([&] { return windowSize(*window) == ui::Size{520, 320}; }));
        rig.turnFor(50ms);
        CHECK(ownViewAtTopLeft(window->nativeHandle()));
    }

    rig.turnFor(50ms);
    CHECK(unitMisuses().empty());
}

TEST_CASE("An Audio Unit whose view can't be made gets the page of its parameters in its window, saying why",
          "[windows]")
{
    ui::messageLoop::initialize();
    UnitRig rig;
    auto plugin = rig.made("VThr");
    PluginError error;
    auto window = PluginWindow::open(plugin, {"Throws"}, error);
    REQUIRE(window != nullptr);
    CHECK(window->editor() == nullptr);
    CHECK(window->parameterPage() != nullptr);
    CHECK(window->editorProblem().message.find("the test view's own exception") != std::string::npos);
}

TEST_CASE("A resizable Audio Unit view's window can be dragged, the view following it, and a fixed one's can't",
          "[windows]")
{
    UnitRig rig;
    PluginError error;
    auto sizable = rig.made("VSiz");
    auto window = openWindow(sizable, error);
    REQUIRE(window != nullptr);
    CHECK(window->isResizable());
    CHECK(dragWindow(*window, {700, 500}) == ui::Size{700, 500});
    CHECK(window->editorSize() == PluginEditorSize{700, 500});
    CHECK(ownViewSize(window->nativeHandle()) == ui::Size{700, 500});
    CHECK(ownViewAtTopLeft(window->nativeHandle()));

    auto fixedUnit = rig.made("VFix");
    auto fixedWindow = openWindow(fixedUnit, error);
    REQUIRE(fixedWindow != nullptr);
    CHECK_FALSE(fixedWindow->isResizable());
    CHECK(dragWindow(*fixedWindow, {700, 500}) == ui::Size{500, 300});
    CHECK(unitMisuses().empty());
}

TEST_CASE("An Audio Unit whose view can't be made opens none, saying why, and goes on working: a factory that throws, "
          "a class its bundle doesn't hold, and a class another plugin's took",
          "[windows]")
{
    UnitRig rig;
    PluginError error;

    auto throws = rig.made("VThr");
    CHECK(openWindow(throws, error) == nullptr);
    CHECK(error.message.find("the test view's own exception") != std::string::npos);
    CHECK(throws->state() == Plugin::State::active);

    auto missing = rig.made("VMis");
    CHECK(openWindow(missing, error) == nullptr);
    CHECK(error.message == "Its editor's class isn't in its bundle.");

    // The twin's view class was taken by the first component's copy, so it is refused rather than shown as the first's.
    auto first = rig.made("VFix");
    auto firstWindow = openWindow(first, error);
    REQUIRE(firstWindow != nullptr);
    auto twin = rig.made("VTwn");
    CHECK(openWindow(twin, error) == nullptr);
    CHECK(error.message == "Its editor's class collides with another plugin's.");

    // One whose factory needs its unit initialized gets an initialized unit.
    auto needs = rig.made("VNei");
    auto needsWindow = openWindow(needs, error);
    INFO(error.message);
    CHECK(needsWindow != nullptr);
    CHECK(unitMisuses().empty());
}

#endif

TEST_CASE("The editor of a plugin stuck in its process call is closed without calling the plugin")
{
    FakeRig rig;
    std::timed_mutex held;
    std::atomic<bool> stick{false}, running{true};
    FakePluginFormat::Script stuck;
    stuck.editor = true;
    stuck.inside = [&](std::string_view call) {
        if (call == "process" && stick)
            std::unique_lock wait(held);
    };
    rig.fake.script("stuck", stuck);

    PluginHost quick{{rig.main.post(), rig.main.isCurrent(), 200ms, [](const std::string&) {}}};
    auto plugin = quick.make(rig.fake, FakePluginFormat::describe("stuck"));
    REQUIRE(rig.main.turnUntil([&] { return plugin->state() != Plugin::State::making; }));
    REQUIRE(plugin->prepare({48000.0, 64, false}));

    Frame frame;
    PluginError error;
    auto editor = PluginEditor::open(plugin, &frame, frame, error);
    REQUIRE(editor != nullptr);

    held.lock();
    stick = true;
    std::thread audio([&] {
        std::vector<float> left(64), right(64);
        float* channels[] = {left.data(), right.data()};
        while (running)
        {
            plugin->process(channels, 2, 64);
            std::this_thread::sleep_for(1ms);
        }
    });
    CHECK(rig.main.turnUntil([&] { return plugin->state() == Plugin::State::failed; }, 3s));

    editor.reset();
    rig.main.settle();
    CHECK(rig.count("stuck", "closeEditor") == 0);

    running = false;
    stick = false;
    held.unlock();
    audio.join();
}
