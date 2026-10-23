#include <symbolengine/core/Json.h>
#include <symbolengine/ui/WebView.h>

#include <choc/gui/choc_MessageLoop.h>

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <stdexcept>
#include <string>

using namespace symbolengine;

namespace
{
const char* const page = R"html(<!doctype html>
<html>
<head><meta charset="utf-8"><script src="symbolengine.js"></script><script src="checks.js"></script></head>
<body>A page for the tests</body>
</html>)html";

// Runs each check in the page and reports what it found, then says it's done.
const char* const checks = R"js(
symbolengine.on("early", data => symbolengine.call("report", "early", data));

(async () => {
    await symbolengine.call("report", "sum", await symbolengine.call("add", 40, 2));
    await symbolengine.call("report", "echo", await symbolengine.call("echo", "café \u{1F3A7}"));
    try {
        await symbolengine.call("fails");
        await symbolengine.call("report", "fails", "it didn't");
    } catch (error) {
        await symbolengine.call("report", "fails", error.message);
    }
    try {
        await symbolengine.call("nothing");
    } catch (error) {
        await symbolengine.call("report", "nothing", error.message);
    }
    const missing = await fetch("missing.txt").then(r => r.status, () => "refused");
    await symbolengine.call("report", "missing", String(missing));
    await symbolengine.call("done");
})();
)js";

// A web view with the page above, run until the page says it's done or twenty seconds pass.
std::map<std::string, Value> runPage()
{
    std::map<std::string, Value> reports;
    bool timedOut = false;

    ui::WebView view({.resources = [](std::string_view path) -> std::optional<ui::Resource> {
        if (path == "/index.html")
            return ui::Resource{page, ""};
        if (path == "/checks.js")
            return ui::Resource{checks, ""};
        return std::nullopt;
    }});
    REQUIRE(view.available());
    REQUIRE(view.nativeView() != nullptr);

    view.expose("add", [](const Value& arguments) { return Value(arguments[0].asInt() + arguments[1].asInt()); });
    view.expose("echo", [](const Value& arguments) { return arguments[0]; });
    view.expose("fails", [](const Value&) -> Value { throw std::runtime_error("the native side failed"); });
    view.expose("report", [&](const Value& arguments) {
        reports[arguments[0].asString()] = arguments[1];
        return Value();
    });
    view.expose("done", [&](const Value&) {
        choc::messageloop::stop();
        return Value();
    });

    // Sent before the page has loaded, so it is held until the page is ready.
    view.send("early", Object{{"held", true}});
    CHECK_FALSE(view.pageReady());

    choc::messageloop::Timer timeout(20000, [&] {
        timedOut = true;
        choc::messageloop::stop();
        return false;
    });
    choc::messageloop::run();

    CHECK_FALSE(timedOut);
    CHECK(view.pageReady());
    return reports;
}
}

TEST_CASE("Files are given MIME types by their names")
{
    CHECK(ui::mimeTypeFor("/index.html") == "text/html");
    CHECK(ui::mimeTypeFor("/app.JS") == "text/javascript");
    CHECK(ui::mimeTypeFor("/fonts/Jost.woff2") == "font/woff2");
    CHECK(ui::mimeTypeFor("/images/knob.svg") == "image/svg+xml");
    CHECK(ui::mimeTypeFor("/data") == "application/octet-stream");
}

TEST_CASE("A page loads its files and talks to the native side both ways")
{
    choc::messageloop::initialise();
    const auto reports = runPage();

    REQUIRE(reports.contains("sum"));
    CHECK(reports.at("sum") == Value(42));
    CHECK(reports.at("echo").stringOr("") == "caf\xC3\xA9 \xF0\x9F\x8E\xA7");
    CHECK(reports.at("fails").stringOr("") == "the native side failed");
    CHECK(reports.at("nothing").stringOr("") == "there is no native function called nothing");
    CHECK(reports.at("missing").stringOr("") != "200");
    REQUIRE(reports.contains("early"));
    CHECK(reports.at("early")["held"].boolOr(false));
}
