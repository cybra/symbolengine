// A plugin's editor opened in a host's window through CLAP's gui extension, on Mac and Windows: its page reads and
// changes the parameters, and the host hears the change as automation and sends one back.

#include "TestHost.h"
#include "TestPlugins.h"

#include <symbolengine/plugin.h>

#include <choc/gui/choc_MessageLoop.h>
#if defined(__APPLE__)
    #include <choc/platform/choc_ObjectiveCHelpers.h>
#else
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#endif

#include <catch2/catch_test_macros.hpp>

#include <symbolengine/core/Files.h>
#include <symbolengine/core/Json.h>
#include <symbolengine/ui/MessageLoop.h>
#include <symbolengine/ui/Native.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <map>
#include <random>
#include <thread>
#include <tuple>
#include <vector>

using namespace symbolengine;
using namespace symbolengine::plugin;
using tests::TestGain;

namespace
{
#if defined(__APPLE__)
const char* const windowApi = CLAP_WINDOW_API_COCOA;
#else
const char* const windowApi = CLAP_WINDOW_API_WIN32;
#endif

// A view for the editor to go in, as a host's window has, never shown on screen.
struct ParentView
{
    ParentView()
    {
#if defined(__APPLE__)
        using namespace choc::objc;
        view = call<id>(callClass<id>("NSView", "alloc"), "initWithFrame:", CGRect{{0, 0}, {320, 200}});
#else
        view = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 320, 200, nullptr, nullptr, GetModuleHandleW(nullptr),
                               nullptr);
#endif
    }

    ~ParentView()
    {
#if defined(__APPLE__)
        choc::objc::call<void>(static_cast<id>(view), "release");
#else
        DestroyWindow(static_cast<HWND>(view));
#endif
    }

    ParentView(const ParentView&) = delete;
    ParentView& operator=(const ParentView&) = delete;

    void* view = nullptr;
};

// The events a plugin sends the host, as (type, parameter, value).
struct Collected
{
    std::vector<std::tuple<std::uint16_t, clap_id, double>> events;
    clap_output_events list{this, [](const clap_output_events* l, const clap_event_header* header) {
                                auto& self = *static_cast<Collected*>(l->ctx);
                                if (header->type == CLAP_EVENT_PARAM_VALUE)
                                {
                                    const auto& value = reinterpret_cast<const clap_event_param_value&>(*header);
                                    self.events.emplace_back(header->type, value.param_id, value.value);
                                }
                                else
                                {
                                    const auto& gesture = reinterpret_cast<const clap_event_param_gesture&>(*header);
                                    self.events.emplace_back(header->type, gesture.param_id, 0.0);
                                }
                                return true;
                            }};
};
}

TEST_CASE("An editor opens in the host's window, and its page and the host change each other's parameters", "[webview]")
{
    TestHost host;
    Made made(host, "test.gain");
    REQUIRE(made.plugin != nullptr);

    const auto& gui = made.extension<clap_plugin_gui>(CLAP_EXT_GUI);
    const auto& params = made.extension<clap_plugin_params>(CLAP_EXT_PARAMS);

    const char* preferred = nullptr;
    bool floating = true;
    REQUIRE(gui.get_preferred_api(made.plugin, &preferred, &floating));
    CHECK(std::string(preferred) == windowApi);
    CHECK_FALSE(floating);
    CHECK(gui.is_api_supported(made.plugin, windowApi, false));
    CHECK_FALSE(gui.is_api_supported(made.plugin, windowApi, true));
    CHECK_FALSE(gui.is_api_supported(made.plugin, CLAP_WINDOW_API_X11, false));

    std::map<std::string, Value> reports;
    Collected told;
    bool timedOut = false;

    TestGain::onEditorOpened = [&](EditorLink& link) {
        link.expose("report", [&](const Value& arguments) {
            reports[arguments[0].asString()] = arguments[1];
            return Value();
        });
        link.expose("done", [&](const Value&) {
            choc::messageloop::stop();
            return Value();
        });
        // The plugin's own function sets Mode to Wide as the user does.
        link.expose("wide", [&link](const Value&) {
            link.beginGesture(2);
            const auto value = link.setParameter(2, 7.0);
            link.endGesture(2);
            return Value(value);
        });
        // The host changes a parameter, as automation does, and collects the editor's changes at the same time.
        link.expose("automate", [&](const Value&) {
            Changes automation;
            automation.add(0, 3, 0.0);
            params.flush(made.plugin, &automation.list, &told.list);
            return Value();
        });
    };

    REQUIRE(gui.create(made.plugin, windowApi, false));

    std::uint32_t width = 0, height = 0;
    REQUIRE(gui.get_size(made.plugin, &width, &height));
    CHECK(width == 320);
    CHECK(height == 200);
    CHECK_FALSE(gui.can_resize(made.plugin));

    ParentView parent;
    clap_window window{};
    window.api = windowApi;
    window.ptr = parent.view;
    REQUIRE(gui.set_parent(made.plugin, &window));
    REQUIRE(gui.show(made.plugin));

    choc::messageloop::Timer timeout(20000, [&] {
        timedOut = true;
        choc::messageloop::stop();
        return false;
    });
    choc::messageloop::run();

    gui.hide(made.plugin);
    gui.destroy(made.plugin);
    TestGain::onEditorOpened = nullptr;

    REQUIRE_FALSE(timedOut);

    CHECK(reports["gain"]["name"].stringOr("") == "Gain");
    CHECK(reports["gain"]["text"].stringOr("") == "0.0 dB");
    CHECK(reports["gain"]["minimum"] == Value(-60));
    CHECK(reports["modes"] == Value(Array{"Narrow", "Wide"}));
    CHECK(reports["typed"].stringOr("") == "-3.3 dB");
    CHECK(reports["changed"].stringOr("") == "-12.0 dB");
    CHECK(reports["native"] == Value(1));
    CHECK(reports["heard"].stringOr("") == "Wide");
    CHECK(reports["automated"].stringOr("") == "Off");

    // The plugin has the value the page set, and the host was asked to collect the change and was told of it as one
    // gesture.
    CHECK(TestGain::last->gain.get() == -12.0);
    CHECK(host.flushRequests >= 1);
    using Event = std::tuple<std::uint16_t, clap_id, double>;
    CHECK(told.events == std::vector<Event>{{CLAP_EVENT_PARAM_GESTURE_BEGIN, 1, 0.0},
                                            {CLAP_EVENT_PARAM_VALUE, 1, -12.0},
                                            {CLAP_EVENT_PARAM_GESTURE_END, 1, 0.0},
                                            {CLAP_EVENT_PARAM_GESTURE_BEGIN, 2, 0.0},
                                            {CLAP_EVENT_PARAM_VALUE, 2, 1.0},
                                            {CLAP_EVENT_PARAM_GESTURE_END, 2, 0.0}});
    CHECK(TestGain::last->mode.get() == 1.0);

    // It ticked as often as it asked, 5 times a second, well below the 30 it would have by default; a timer can fire
    // later than asked on a busy machine, never sooner.
    const auto& ticks = TestGain::last->ticks;
    REQUIRE(ticks.size() >= 2);
    const std::chrono::duration<double> span = ticks.back() - ticks.front();
    CHECK(static_cast<double>(ticks.size() - 1) / span.count() < 10.0);
}

TEST_CASE("A resizable editor asks the host for sizes within its limits, and opens again at the size it was given", "[webview]")
{
    std::random_device random;
    const auto folder = std::filesystem::temp_directory_path() / ("symbolengine-editor-" + std::to_string(random()));
    tests::TestLatency::sizeFile = folder / "Editor.json";

    std::map<std::string, Value> reports;
    tests::TestLatency::onEditorOpened = [&](EditorLink& link) {
        link.expose("report", [&](const Value& arguments) {
            reports[arguments[0].asString()] = arguments[1];
            return Value();
        });
        link.expose("done", [&](const Value&) {
            choc::messageloop::stop();
            return Value();
        });
    };

    TestHost host;
    std::uint32_t width = 0, height = 0;
    {
        Made made(host, "test.latency");
        const auto& gui = made.extension<clap_plugin_gui>(CLAP_EXT_GUI);
        REQUIRE(gui.create(made.plugin, windowApi, false));
        CHECK(gui.can_resize(made.plugin));

        REQUIRE(gui.get_size(made.plugin, &width, &height));
        CHECK(width == 400);
        CHECK(height == 240);

        width = height = 10;
        REQUIRE(gui.adjust_size(made.plugin, &width, &height));
        CHECK(width == 300);
        CHECK(height == 200);
        width = height = 5000;
        REQUIRE(gui.adjust_size(made.plugin, &width, &height));
        CHECK(width == 900);
        CHECK(height == 600);

        // The host resizes it, as when the user drags the window's corner.
        CHECK(gui.set_size(made.plugin, 450, 260));
        CHECK(tests::TestLatency::last->resized == std::pair<std::uint32_t, std::uint32_t>{450, 260});
        CHECK_FALSE(gui.set_size(made.plugin, 10, 10));

        // The page asks for sizes, held to the limits.
        ParentView parent;
        clap_window window{};
        window.api = windowApi;
        window.ptr = parent.view;
        REQUIRE(gui.set_parent(made.plugin, &window));
        REQUIRE(gui.show(made.plugin));

        bool timedOut = false;
        choc::messageloop::Timer timeout(20000, [&] {
            timedOut = true;
            choc::messageloop::stop();
            return false;
        });
        choc::messageloop::run();
        REQUIRE_FALSE(timedOut);

        CHECK(reports["tooSmall"] == Value(Object{{"width", 300}, {"height", 200}, {"agreed", true}}));
        CHECK(reports["larger"] == Value(Object{{"width", 500}, {"height", 320}, {"agreed", true}}));
        CHECK(reports["halfPoint"] == Value(Object{{"width", 601}, {"height", 300}, {"agreed", true}}));
        using Size = std::pair<std::uint32_t, std::uint32_t>;
        CHECK(host.resizeRequests == std::vector<Size>{{300, 200}, {500, 320}, {601, 300}, {500, 320}, {600, 370}});
        REQUIRE(gui.get_size(made.plugin, &width, &height));
        CHECK(width == 600);
        CHECK(height == 370);

        // Opened again on the same instance, it keeps its size.
        gui.destroy(made.plugin);
        REQUIRE(gui.create(made.plugin, windowApi, false));
        REQUIRE(gui.get_size(made.plugin, &width, &height));
        CHECK(width == 600);
        CHECK(height == 370);
        gui.destroy(made.plugin);
    }

    // A new instance opens at the size kept in the file, and the plugin can set its own.
    CHECK(json::parse(files::read(tests::TestLatency::sizeFile)) == Value(Object{{"width", 600}, {"height", 370}}));
    {
        Made made(host, "test.latency");
        const auto& gui = made.extension<clap_plugin_gui>(CLAP_EXT_GUI);
        REQUIRE(gui.create(made.plugin, windowApi, false));
        REQUIRE(gui.get_size(made.plugin, &width, &height));
        CHECK(width == 600);
        CHECK(height == 370);

        tests::TestLatency::last->setEditorSize(640, 480);
        REQUIRE(gui.get_size(made.plugin, &width, &height));
        CHECK(width == 640);
        CHECK(height == 480);
        CHECK(host.resizeRequests.back() == std::pair<std::uint32_t, std::uint32_t>{640, 480});
        gui.destroy(made.plugin);
    }

    tests::TestLatency::onEditorOpened = nullptr;
    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
}

namespace
{
using Event = std::tuple<std::uint16_t, clap_id, double>;

// A plugin with its editor open, for checking what the editor tells the host, the editor's link to it at hand.
struct OpenEditor
{
    OpenEditor() : made(host, "test.gain")
    {
        TestGain::onEditorOpened = [this](EditorLink& given) { link = &given; };
        REQUIRE(gui().create(made.plugin, windowApi, false));
        REQUIRE(link != nullptr);
    }

    ~OpenEditor()
    {
        TestGain::onEditorOpened = nullptr;
        close();
    }

    const clap_plugin_gui& gui() const { return made.extension<clap_plugin_gui>(CLAP_EXT_GUI); }

    void close()
    {
        if (!closed)
            gui().destroy(made.plugin);
        closed = true;
    }

    // What the plugin has for the host, as it collects when it isn't processing.
    std::vector<Event> collected()
    {
        Changes none;
        Collected told;
        made.extension<clap_plugin_params>(CLAP_EXT_PARAMS).flush(made.plugin, &none.list, &told.list);
        return told.events;
    }

    TestHost host;
    Made made;
    EditorLink* link = nullptr;
    bool closed = false;
};

std::size_t countOf(const std::vector<Event>& events, std::uint16_t type)
{
    return static_cast<std::size_t>(
        std::count_if(events.begin(), events.end(), [&](const Event& event) { return std::get<0>(event) == type; }));
}
}

TEST_CASE("An editor's changes reach a host that is processing, which may have stopped rendering", "[webview]")
{
    OpenEditor open;
    REQUIRE(open.made.plugin->activate(open.made.plugin, 44100.0, 1, 256));
    REQUIRE(open.made.plugin->start_processing(open.made.plugin));

    const auto before = open.host.flushRequests;
    open.link->setParameter(1, -6.0);
    CHECK(open.host.flushRequests > before);

    open.made.plugin->stop_processing(open.made.plugin);
    open.made.plugin->deactivate(open.made.plugin);
}

TEST_CASE("A gesture left open is ended when the editor closes, and only a gesture begun is ended", "[webview]")
{
    OpenEditor open;

    // Nothing to end, and a second beginning of what has begun.
    open.link->endGesture(2);
    open.link->beginGesture(1);
    open.link->beginGesture(1);
    open.link->setParameter(1, -6.0);
    open.close();

    CHECK(open.collected() == std::vector<Event>{{CLAP_EVENT_PARAM_GESTURE_BEGIN, 1, 0.0},
                                                 {CLAP_EVENT_PARAM_VALUE, 1, -6.0},
                                                 {CLAP_EVENT_PARAM_GESTURE_END, 1, 0.0}});
}

TEST_CASE("However many changes a host hasn't collected, every gesture begun is ended", "[webview]")
{
    SECTION("one long gesture")
    {
        OpenEditor open;
        open.link->beginGesture(1);
        for (int i = 0; i < 10000; ++i)
            open.link->setParameter(1, -static_cast<double>(i % 60));
        open.link->endGesture(1);

        const auto events = open.collected();
        CHECK(countOf(events, CLAP_EVENT_PARAM_GESTURE_BEGIN) == 1);
        CHECK(countOf(events, CLAP_EVENT_PARAM_GESTURE_END) == 1);
        REQUIRE_FALSE(events.empty());
        CHECK(std::get<0>(events.back()) == CLAP_EVENT_PARAM_GESTURE_END);
    }

    SECTION("many short ones")
    {
        OpenEditor open;
        for (int i = 0; i < 5000; ++i)
        {
            open.link->beginGesture(1);
            open.link->setParameter(1, -1.0);
            open.link->setParameter(1, -2.0);
            open.link->endGesture(1);
        }

        const auto events = open.collected();
        const auto begun = countOf(events, CLAP_EVENT_PARAM_GESTURE_BEGIN);
        CHECK(begun > 0);
        CHECK(countOf(events, CLAP_EVENT_PARAM_GESTURE_END) == begun);
    }

    SECTION("nothing but beginnings and ends")
    {
        OpenEditor open;
        for (int i = 0; i < 5000; ++i)
        {
            open.link->beginGesture(1);
            open.link->endGesture(1);
        }

        const auto events = open.collected();
        const auto begun = countOf(events, CLAP_EVENT_PARAM_GESTURE_BEGIN);
        CHECK(begun > 0);
        CHECK(countOf(events, CLAP_EVENT_PARAM_GESTURE_END) == begun);
    }

    SECTION("left open as the editor closes, with the queue full")
    {
        OpenEditor open;
        open.link->beginGesture(1);
        for (int i = 0; i < 10000; ++i)
            open.link->setParameter(1, -static_cast<double>(i % 60));
        open.close();

        const auto events = open.collected();
        CHECK(countOf(events, CLAP_EVENT_PARAM_GESTURE_END) == 1);
    }
}

TEST_CASE("An exception from a plugin's editor code never reaches the host", "[webview]")
{
    OpenEditor open;

    // Its tick keeps being called, each one throwing.
    TestGain::failToTick = true;
    const auto before = TestGain::last->ticks.size();
    ui::messageLoop::runUntil([&] { return TestGain::last->ticks.size() >= before + 3; },
                              std::chrono::milliseconds(5000));
    TestGain::failToTick = false;
    CHECK(TestGain::last->ticks.size() >= before + 3);

    TestGain::failToClose = true;
    open.close();
    TestGain::failToClose = false;
}

TEST_CASE("An editor's timing still runs after a worker has asked for a call before it opened", "[webview]")
{
    TestHost host;
    Made made(host, "test.gain");

    // As a plugin's worker might, as soon as the plugin is made.
    bool called = false;
    std::thread([&] { ui::messageLoop::callAsync([&] { called = true; }); }).join();

    const auto& gui = made.extension<clap_plugin_gui>(CLAP_EXT_GUI);
    REQUIRE(gui.create(made.plugin, windowApi, false));
    const auto ticking =
        ui::messageLoop::runUntil([&] { return called && TestGain::last->ticks.size() >= 2; }, std::chrono::seconds(5));
    gui.destroy(made.plugin);

    CHECK(called);
    CHECK(ticking);
}

#if defined(_WIN32)
TEST_CASE("An editor told its scale after it is made still opens to fit the screen", "[webview]")
{
    std::random_device random;
    const auto folder = std::filesystem::temp_directory_path() / ("symbolengine-scale-" + std::to_string(random()));
    std::filesystem::create_directories(folder);
    tests::TestLatency::sizeFile = folder / "Editor.json";
    files::writeSafely(tests::TestLatency::sizeFile, R"({"width": 900, "height": 600})");

    TestHost host;
    {
        Made made(host, "test.latency");
        const auto& gui = made.extension<clap_plugin_gui>(CLAP_EXT_GUI);
        REQUIRE(gui.create(made.plugin, windowApi, false));

        // Enough pixels to a point for its largest size not to fit.
        const auto screen = ui::native::workAreaAtPointer();
        const auto scale = std::max(screen.width / 900.0, screen.height / 600.0) * 1.5;
        REQUIRE(gui.set_scale(made.plugin, scale));

        std::uint32_t width = 0, height = 0;
        REQUIRE(gui.get_size(made.plugin, &width, &height));
        CHECK(
            (width <= static_cast<std::uint32_t>(screen.width) || height <= static_cast<std::uint32_t>(screen.height)));
        gui.destroy(made.plugin);
    }

    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
}
#endif
