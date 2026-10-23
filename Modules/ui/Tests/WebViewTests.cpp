#include <symbolengine/core/Json.h>
#include <symbolengine/ui/WebView.h>

#include <choc/gui/choc_MessageLoop.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <map>
#include <random>
#include <stdexcept>
#include <string>

using namespace symbolengine;

namespace
{
// A page that loads the bridge and then the script given.
std::string pageWith(std::string_view script)
{
    return std::string(R"html(<!doctype html>
<html>
<head><meta charset="utf-8"><script src="symbolengine.js"></script><script>)html") +
           std::string(script) + "</script></head><body>A page for the tests</body></html>";
}

// Runs a page until it calls "done" or twenty seconds pass, and gives what it reported with "report".
std::map<std::string, Value> runPage(ui::WebView::Options options, std::function<void(ui::WebView&)> before = {})
{
    choc::messageloop::initialise();

    std::map<std::string, Value> reports;
    bool timedOut = false;

    ui::WebView view(std::move(options));
    REQUIRE(view.available());
    REQUIRE(view.nativeView() != nullptr);

    view.expose("report", [&](const Value& arguments) {
        reports[arguments[0].asString()] = arguments[1];
        return Value();
    });
    view.expose("done", [&](const Value&) {
        choc::messageloop::stop();
        return Value();
    });
    if (before)
        before(view);

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

ui::Resources onePage(std::string page)
{
    return [page = std::move(page)](std::string_view path) -> std::optional<ui::Resource> {
        if (path == "/index.html")
            return ui::Resource{page, ""};
        return std::nullopt;
    };
}

// Runs each check in the page and reports what it found.
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

std::string unique(std::string_view stem)
{
    std::random_device random;
    return std::string(stem) + "-" + std::to_string(random());
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
    const auto reports = runPage({.resources = onePage(pageWith(checks))}, [](ui::WebView& view) {
        view.expose("add", [](const Value& arguments) { return Value(arguments[0].asInt() + arguments[1].asInt()); });
        view.expose("echo", [](const Value& arguments) { return arguments[0]; });
        view.expose("fails", [](const Value&) -> Value { throw std::runtime_error("the native side failed"); });

        // Sent before the page has loaded, so it is held until the page is ready.
        view.send("early", Object{{"held", true}});
        CHECK_FALSE(view.pageReady());
    });

    REQUIRE(reports.contains("sum"));
    CHECK(reports.at("sum") == Value(42));
    CHECK(reports.at("echo").stringOr("") == "caf\xC3\xA9 \xF0\x9F\x8E\xA7");
    CHECK(reports.at("fails").stringOr("") == "the native side failed");
    CHECK(reports.at("nothing").stringOr("") == "there is no native function called nothing");
    CHECK(reports.at("missing").stringOr("") != "200");
    REQUIRE(reports.contains("early"));
    CHECK(reports.at("early")["held"].boolOr(false));
}

TEST_CASE("Pages with different names keep what they store apart, and pages with the same name share it")
{
    // Names new to this run, so what an earlier run stored can't answer for this one.
    const auto first = unique("first"), second = unique("second");

    const auto store = pageWith(R"js(
        localStorage.setItem("kept", "stored by the first page");
        symbolengine.call("done");
    )js");
    const auto read = pageWith(R"js(
        symbolengine.call("report", "kept", localStorage.getItem("kept"))
            .then(() => localStorage.removeItem("kept"))
            .then(() => symbolengine.call("done"));
    )js");

    runPage({.resources = onePage(store), .name = first});
    const auto other = runPage({.resources = onePage(read), .name = second});
    const auto same = runPage({.resources = onePage(read), .name = first});

    CHECK(other.at("kept").isNull());
    CHECK(same.at("kept").stringOr("") == "stored by the first page");
}

#if defined(_WIN32)
TEST_CASE("WebView2 keeps its data in the folder it is given")
{
    const auto folder = std::filesystem::temp_directory_path() / unique("symbolengine-webview");
    runPage({.resources = onePage(pageWith(R"js(symbolengine.call("done");)js")), .dataFolder = folder});

    CHECK(std::filesystem::exists(folder / "EBWebView"));

    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
}
#endif
