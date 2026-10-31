#include <symbolengine/core/Files.h>
#include <symbolengine/core/Json.h>
#include <symbolengine/ui/MessageLoop.h>
#include <symbolengine/ui/Native.h>
#include <symbolengine/ui/WebView.h>
#include <symbolengine/ui/Window.h>

#include <choc/gui/choc_MessageLoop.h>

#include <catch2/catch_test_macros.hpp>

#if defined(__APPLE__)
    #include <CoreGraphics/CoreGraphics.h>
    #include <ImageIO/ImageIO.h>
#elif defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#endif
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>

#include <filesystem>
#include <map>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

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

// Runs a page until it calls "done" and is ready, or twenty seconds pass, and gives what it reported with "report". A
// page may call "done" as it loads, before it has said it is ready.
std::map<std::string, Value> runPage(ui::WebView::Options options, std::function<void(ui::WebView&)> before = {})
{
    choc::messageloop::initialise();

    std::map<std::string, Value> reports;
    bool timedOut = false, finished = false;

    ui::WebView view(std::move(options));
    REQUIRE(view.available());
    REQUIRE(view.nativeView() != nullptr);

    view.expose("report", [&](const Value& arguments) {
        reports[arguments[0].asString()] = arguments[1];
        return Value();
    });
    view.expose("done", [&](const Value&) {
        finished = true;
        return Value();
    });
    if (before)
        before(view);

    // Stopped once, as each stop leaves the loop a message of its own.
    choc::messageloop::Timer look(10, [&] {
        if (!finished || !view.pageReady())
            return true;
        choc::messageloop::stop();
        return false;
    });

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

[[maybe_unused]] void setVariable(const char* name, const char* value)
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

TEST_CASE("A page loads its files and talks to the native side both ways", "[webview]")
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

TEST_CASE("A function's answer can come later, after the call has returned, and only the first counts", "[webview]")
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

TEST_CASE("A script run in the page gives what it came to, or what went wrong", "[webview]")
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

    INFO("the sum's error: " << sumError);
    CHECK(sum == Value(3));
    CHECK(sumError.empty());
    CHECK(object["name"].stringOr("") == "caf\xC3\xA9");
    CHECK(json::write(object["list"]) == R"([1,"x",null])");
    CHECK(thrown.isNull());
#if defined(__APPLE__)
    CHECK(!thrownError.empty());   // WebView2 gives a script that threw as null, without saying why
#endif
}

TEST_CASE("Pages with different names keep what they store apart, and pages with the same name share it", "[webview]")
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
TEST_CASE("WebView2 keeps its data in the folder it is given", "[webview]")
{
    const auto folder = std::filesystem::temp_directory_path() / unique("symbolengine-webview");
    runPage({.resources = onePage(pageWith(R"js(symbolengine.call("done");)js")), .dataFolder = folder});

    CHECK(std::filesystem::exists(folder / "EBWebView"));

    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
}
#endif

#if defined(__APPLE__)
TEST_CASE("A snapshot saves the page as it is drawn", "[webview]")
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

namespace
{
// A native timer for a page to wait on, as one no window shows has its own timers held back.
void exposeWait(ui::WebView& view)
{
    view.exposeLater("wait", [](const Value& arguments, ui::WebView::Reply reply) {
        ui::messageLoop::callAfter(static_cast<std::uint32_t>(arguments[0].intOr(0)),
                                   [reply = std::move(reply)] { reply(Value()); });
    });
}

// Nowhere there is, so nothing is ever fetched, though going there would take the page away. about:blank wouldn't show
// this: a web view goes there whatever it is told.
constexpr std::string_view elsewhere = "https://symbolengine.invalid/";

// A script trying to leave, and reporting where it is once it has had the time.
const char* const leaving = R"js(
(async () => {
    await new Promise(loaded => addEventListener("DOMContentLoaded", loaded));
    window.open("https://symbolengine.invalid/window");
    const frame = document.createElement("iframe");
    frame.src = "https://symbolengine.invalid/frame";
    document.body.append(frame);
    await symbolengine.call("wait", 200);
    location.href = "https://symbolengine.invalid/";
    await symbolengine.call("wait", 500);
    await symbolengine.call("report", "at", location.href);
    await symbolengine.call("done");
})();
)js";

bool includes(const std::vector<std::string>& addresses, const std::string& address)
{
    return std::find(addresses.begin(), addresses.end(), address) != addresses.end();
}
}

TEST_CASE("A page goes only where its program allows", "[webview]")
{
    SECTION("Without a function to ask, nowhere")
    {
        const auto reports = runPage({.resources = onePage(pageWith(leaving))}, exposeWait);
        REQUIRE(reports.contains("at"));
        CHECK_FALSE(reports.at("at").stringOr("").starts_with(elsewhere));
    }

    SECTION("With one, where it says, as asked of the page and its frames")
    {
        std::vector<std::string> asked;
        const auto reports = runPage({.resources = onePage(pageWith(leaving)),
                                      .allowAddress =
                                          [&](std::string_view address) {
                                              asked.emplace_back(address);
                                              return false;
                                          }},
                                     exposeWait);
        REQUIRE(reports.contains("at"));
        CHECK_FALSE(reports.at("at").stringOr("").starts_with(elsewhere));
        CHECK(includes(asked, std::string(elsewhere) + "frame"));
        CHECK(includes(asked, std::string(elsewhere)));
    }

    SECTION("And what it allows, it goes to")
    {
        ui::messageLoop::initialize();
        bool asked = false;
        ui::WebView view({.resources = onePage(pageWith(R"js(
window.stayed = "yes";
symbolengine.call("wait", 100).then(() => { location.href = "https://symbolengine.invalid/"; });
)js")),
                          .allowAddress = [&](std::string_view address) {
                              asked = asked || address == elsewhere;
                              return address == elsewhere;
                          }});
        REQUIRE(view.available());
        exposeWait(view);

        std::string stayed = "yes";
        const auto went = ui::messageLoop::runUntil(
            [&] {
                if (view.pageReady() && asked)
                    view.run("String(window.stayed)",
                             [&](const Value& value, const std::string&) { stayed = value.stringOr(""); });
                return stayed != "yes";
            },
            std::chrono::seconds(20));
        CHECK(went);
    }
}

TEST_CASE("A page can only have a browser open a web address", "[webview]")
{
    const auto reports = runPage({.resources = onePage(pageWith(R"js(
(async () => {
    await symbolengine.call("report", "file", await symbolengine.openLink("file:///etc/hosts"));
    await symbolengine.call("report", "script", await symbolengine.openLink("javascript:void(0)"));
    await symbolengine.call("done");
})();
)js"))});
    REQUIRE(reports.contains("file"));
    CHECK(reports.at("file") == Value(false));
    CHECK(reports.at("script") == Value(false));
}

TEST_CASE("A file with a space in its name is found by the address that spells it", "[webview]")
{
    const auto page = pageWith(R"js(
(async () => {
    const spaced = await fetch("my%20file.txt");
    await symbolengine.call("report", "spaced", [spaced.status, await spaced.text()]);
    await symbolengine.call("done");
})();
)js");
    const auto reports = runPage({.resources = [page](std::string_view path) -> std::optional<ui::Resource> {
        if (path == "/index.html")
            return ui::Resource{page, ""};
        if (path == "/my file.txt")
            return ui::Resource{"spaced", "text/plain"};
        return std::nullopt;
    }});
    REQUIRE(reports.contains("spaced"));
    CHECK(reports.at("spaced") == Value(Array{200, "spaced"}));
}

TEST_CASE("A page can ask for part of a file, as an audio element does", "[webview]")
{
    const auto page = pageWith(R"js(
(async () => {
    const ask = async range => {
        const answer = await fetch("data.bin", range ? { headers: { Range: range } } : {});
        return [answer.status, await answer.text(), answer.headers.get("Content-Range") ?? "", answer.headers.get("Accept-Ranges") ?? ""];
    };
    await symbolengine.call("report", "whole", await ask(""));
    await symbolengine.call("report", "middle", await ask("bytes=2-5"));
    await symbolengine.call("report", "toTheEnd", await ask("bytes=7-"));
    await symbolengine.call("report", "lastThree", await ask("bytes=-3"));
    await symbolengine.call("report", "past", await ask("bytes=8-100"));
    await symbolengine.call("report", "outside", await ask("bytes=20-30"));
    await symbolengine.call("done");
})();
)js");

    const auto reports = runPage({.resources = [page](std::string_view path) -> std::optional<ui::Resource> {
        if (path == "/index.html")
            return ui::Resource{page, ""};
        if (path == "/data.bin")
            return ui::Resource{"0123456789", "application/octet-stream"};
        return std::nullopt;
    }});

    REQUIRE(reports.contains("whole"));
    CHECK(reports.at("whole") == Value(Array{200, "0123456789", "", "bytes"}));
    CHECK(reports.at("middle") == Value(Array{206, "2345", "bytes 2-5/10", "bytes"}));
    CHECK(reports.at("toTheEnd") == Value(Array{206, "789", "bytes 7-9/10", "bytes"}));
    CHECK(reports.at("lastThree") == Value(Array{206, "789", "bytes 7-9/10", "bytes"}));
    CHECK(reports.at("past") == Value(Array{206, "89", "bytes 8-9/10", "bytes"}));
    CHECK(reports.at("outside") == Value(Array{416, "", "bytes */10", "bytes"}));
}

TEST_CASE("An audio element plays a page's own file", "[webview]")
{
    // A second of silence at 8000 Hz.
    std::string tone = "RIFF";
    const auto number = [&](std::uint32_t value, int bytes) {
        for (int i = 0; i < bytes; ++i)
            tone += static_cast<char>((value >> (8 * i)) & 0xff);
    };
    number(36 + 16000, 4);
    tone += "WAVEfmt ";
    number(16, 4);
    number(1, 2);
    number(1, 2);
    number(8000, 4);
    number(16000, 4);
    number(2, 2);
    number(16, 2);
    tone += "data";
    number(16000, 4);
    tone.append(16000, '\0');

    const auto page = pageWith(R"js(
(async () => {
    const audio = new Audio("tone.wav");
    const length = await new Promise(found => {
        audio.onloadedmetadata = () => found(audio.duration);
        audio.onerror = () => found(-1);
        symbolengine.call("wait", 8000).then(() => found(-2));
    });
    await symbolengine.call("report", "audio", length);
    await symbolengine.call("done");
})();
)js");

    // In a window that shows, as a page no window shows loads no media, and never in front.
    ui::messageLoop::initialize();
    ui::Window window({.title = "SymbolEngine audio test", .size = {300, 200}});
    std::map<std::string, Value> reports;
    bool finished = false;
    ui::WebView view({.resources = [page, tone](std::string_view path) -> std::optional<ui::Resource> {
        if (path == "/index.html")
            return ui::Resource{page, ""};
        if (path == "/tone.wav")
            return ui::Resource{tone, "audio/wav"};
        return std::nullopt;
    }});
    REQUIRE(view.available());
    exposeWait(view);
    view.expose("report", [&](const Value& arguments) {
        reports[arguments[0].asString()] = arguments[1];
        return Value();
    });
    view.expose("done", [&](const Value&) {
        finished = true;
        return Value();
    });
    window.setContent(view.nativeView());
    window.setVisible(true);

    CHECK(ui::messageLoop::runUntil([&] { return finished; }, std::chrono::seconds(20)));
    window.setVisible(false);
    REQUIRE(reports.contains("audio"));
    CHECK(reports.at("audio").numberOr(0.0) > 0.9);
    CHECK(reports.at("audio").numberOr(0.0) < 1.1);
}

#if defined(_WIN32)
TEST_CASE("Without a WebView2 runtime to use, a web view says it isn't available", "[webview]")
{
    std::random_device random;
    const auto empty = std::filesystem::temp_directory_path() / ("symbolengine-no-runtime-" + std::to_string(random()));
    std::filesystem::create_directories(empty);
    REQUIRE(SetEnvironmentVariableW(L"WEBVIEW2_BROWSER_EXECUTABLE_FOLDER", empty.c_str()));

    ui::messageLoop::initialize();
    {
        ui::WebView view({.resources = onePage(pageWith(""))});
        ui::messageLoop::runUntil([&] { return !view.available(); }, std::chrono::seconds(20));
        CHECK_FALSE(view.available());

        // What is sent to it goes nowhere, rather than building up.
        for (int i = 0; i < 100; ++i)
            view.send("nobody", Object{{"i", i}});
    }

    SetEnvironmentVariableW(L"WEBVIEW2_BROWSER_EXECUTABLE_FOLDER", nullptr);
    std::error_code ignored;
    std::filesystem::remove_all(empty, ignored);
}
#endif
