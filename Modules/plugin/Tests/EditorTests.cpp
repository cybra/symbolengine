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

#include <filesystem>
#include <map>
#include <random>
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

TEST_CASE("An editor opens in the host's window, and its page and the host change each other's parameters")
{
    choc::messageloop::initialise();

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
    CHECK(reports["automated"].stringOr("") == "Off");

    // The plugin has the value the page set, and the host was asked to collect the change and was told of it as one
    // gesture.
    CHECK(TestGain::last->gain.get() == -12.0);
    CHECK(host.flushRequests >= 1);
    using Event = std::tuple<std::uint16_t, clap_id, double>;
    CHECK(told.events == std::vector<Event>{{CLAP_EVENT_PARAM_GESTURE_BEGIN, 1, 0.0},
                                            {CLAP_EVENT_PARAM_VALUE, 1, -12.0},
                                            {CLAP_EVENT_PARAM_GESTURE_END, 1, 0.0}});
}

TEST_CASE("A resizable editor asks the host for sizes within its limits, and opens again at the size it was given")
{
    choc::messageloop::initialise();

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
        using Size = std::pair<std::uint32_t, std::uint32_t>;
        CHECK(host.resizeRequests == std::vector<Size>{{300, 200}, {500, 320}, {600, 370}});
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
