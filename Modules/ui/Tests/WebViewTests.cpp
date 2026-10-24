#include <symbolengine/core/Files.h>
#include <symbolengine/core/Json.h>
#include <symbolengine/ui/Native.h>
#include <symbolengine/ui/WebView.h>

#include <choc/gui/choc_MessageLoop.h>

#include <catch2/catch_test_macros.hpp>

#if defined(__APPLE__)
    #include <CoreGraphics/CoreGraphics.h>
    #include <ImageIO/ImageIO.h>
#endif
#include <array>
#include <cstdlib>

#include <filesystem>
#include <map>
#include <memory>
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

void setVariable(const char* name, const char* value)
{
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    if (value[0] == '\0')
        unsetenv(name);
    else
        setenv(name, value, 1);
#endif
}

#if defined(__APPLE__)
// The color of the pixel in the middle of a PNG, as red, green and blue from 0 to 255.
std::array<int, 3> centerOf(const std::filesystem::path& png)
{
    std::array<int, 3> color{-1, -1, -1};
    const auto path = png.string();
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, reinterpret_cast<const UInt8*>(path.c_str()),
                                                           static_cast<CFIndex>(path.size()), false);
    CGImageSourceRef source = CGImageSourceCreateWithURL(url, nullptr);
    CGImageRef image = source != nullptr ? CGImageSourceCreateImageAtIndex(source, 0, nullptr) : nullptr;

    if (image != nullptr)
    {
        unsigned char pixel[4] = {};
        CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
        CGContextRef context =
            CGBitmapContextCreate(pixel, 1, 1, 8, 4, space, static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedLast));
        const auto w = static_cast<CGFloat>(CGImageGetWidth(image)), h = static_cast<CGFloat>(CGImageGetHeight(image));
        CGContextDrawImage(context, CGRectMake(-w / 2, -h / 2, w, h), image);
        color = {pixel[0], pixel[1], pixel[2]};
        CGContextRelease(context);
        CGColorSpaceRelease(space);
        CGImageRelease(image);
    }

    if (source != nullptr)
        CFRelease(source);
    CFRelease(url);
    return color;
}
#endif
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

TEST_CASE("A function's answer can come later, after the call has returned, and only the first counts")
{
    const auto page = pageWith(R"js(
(async () => {
    const [now, soon, twice] = await Promise.all([symbolengine.call("now", 1), symbolengine.call("soon", 2),
                                                  symbolengine.call("twice", 3)]);
    await symbolengine.call("report", "answers", [now, soon, twice]);
    try {
        await symbolengine.call("throws");
    } catch (error) {
        await symbolengine.call("report", "throws", error.message);
    }
    await symbolengine.call("done");
})();
)js");

    ui::WebView::Reply waiting;
    std::unique_ptr<choc::messageloop::Timer> answerSoon;
    const auto reports = runPage({.resources = onePage(page)}, [&](ui::WebView& view) {
        // Answered before the function returns, after it, and twice.
        view.exposeLater("now", [](const Value& arguments, ui::WebView::Reply reply) { reply(arguments[0].asInt() * 10); });
        view.exposeLater("soon", [&](const Value& arguments, ui::WebView::Reply reply) {
            waiting = [reply, value = arguments[0].asInt() * 10](const Value&) { reply(value); };
            answerSoon = std::make_unique<choc::messageloop::Timer>(200, [&] {
                waiting(Value());
                return false;
            });
        });
        view.exposeLater("twice", [](const Value& arguments, ui::WebView::Reply reply) {
            reply(arguments[0].asInt() * 10);
            reply(-1);
        });
        view.exposeLater("throws", [](const Value&, ui::WebView::Reply) { throw std::runtime_error("no answer"); });
    });

    REQUIRE(reports.contains("answers"));
    CHECK(json::write(reports.at("answers")) == "[10,20,30]");
    CHECK(reports.at("throws").stringOr("") == "no answer");
}

TEST_CASE("A script run in the page gives what it came to, or what went wrong")
{
    Value sum, object, thrown;
    std::string sumError = "not run", thrownError;
    const auto page = pageWith("symbolengine.call('ready');");

    runPage({.resources = onePage(page)}, [&](ui::WebView& view) {
        view.expose("ready", [&](const Value&) {
            view.run("1 + 2", [&](const Value& result, const std::string& error) {
                sum = result;
                sumError = error;
            });
            view.run("({ name: 'caf\u00e9', list: [1, 'x', null] })", [&](const Value& result, const std::string&) { object = result; });
            view.run("throw new Error('no answer')", [&](const Value& result, const std::string& error) {
                thrown = result;
                thrownError = error;
                choc::messageloop::stop();
            });
            return Value();
        });
        view.expose("done", [](const Value&) { return Value(); });
    });

    CHECK(sum == Value(3));
    CHECK(sumError.empty());
    CHECK(json::write(object) == R"({"name":"café","list":[1,"x",null]})");
    CHECK(thrown.isNull());
    CHECK(!thrownError.empty());
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

#if defined(__APPLE__)
TEST_CASE("A snapshot saves the page as it is drawn")
{
    const auto png = std::filesystem::temp_directory_path() / (unique("symbolengine-snapshot") + ".png");
    const auto red = pageWith(R"js(
        document.addEventListener("DOMContentLoaded", () => {
            document.body.style.cssText = "margin:0; background:#ff0000; width:100vw; height:100vh";
            setTimeout(() => symbolengine.call("snap"), 300);
        });
    )js");

    bool written = false;
    runPage({.resources = onePage(red)}, [&](ui::WebView& view) {
        ui::native::resize(view.nativeView(), 200, 150);   // made at no size, as a host would size it
        view.expose("snap", [&](const Value&) {
            view.snapshot(png, [&](bool done) {
                written = done;
                choc::messageloop::stop();
            });
            return Value();
        });
    });

    REQUIRE(written);
    const auto color = centerOf(png);
    CHECK(color[0] > 240);   // red
    CHECK(color[1] < 20);
    CHECK(color[2] < 20);

    std::error_code ignored;
    std::filesystem::remove(png, ignored);
}
#endif

// The live-files folder below is made by CMake in the build folder, and built in as liveTestFiles.
std::optional<std::string_view> liveTestFiles(std::string_view path);

#if !defined(NDEBUG)
TEST_CASE("In a debug build, files built in can be read live from the folder they came from")
{
    const auto file = std::filesystem::path(SYMBOLENGINE_LIVE_TEST_FOLDER) / "greeting.txt";
    const auto builtIn = files::read(file);
    REQUIRE(liveTestFiles("/greeting.txt") == std::optional<std::string_view>(builtIn));

    files::writeSafely(file, "changed on disk");
    CHECK(liveTestFiles("/greeting.txt") == std::optional<std::string_view>(builtIn));   // not live yet

    setVariable("SYMBOLENGINE_LIVE_FILES", "1");
    CHECK(liveTestFiles("/greeting.txt") == std::optional<std::string_view>("changed on disk"));
    CHECK_FALSE(liveTestFiles("/../greeting.txt").has_value());
    setVariable("SYMBOLENGINE_LIVE_FILES", "");

    files::writeSafely(file, builtIn);
}
#endif
