// A desktop program on SymbolEngine: one window holding a web page, the program's menu bar on a Mac, one instance at
// a time, and the files the system or a second instance asks it to open.
#include <symbolengine/app.h>
#include <symbolengine/core/Paths.h>
#include <symbolengine/ui/MenuBar.h>
#include <symbolengine/ui/WebView.h>
#include <symbolengine/ui/Window.h>

#include <optional>
#include <string>
#include <string_view>

// The page's files, built in by symbolengine_add_files.
std::optional<std::string_view> helloFiles(std::string_view path);

using namespace symbolengine;

int main(int argc, char** argv)
{
    Application app({.name = SYMBOLENGINE_APP_NAME, .identifier = "dev.symbolengine.examples.hello"}, argc, argv);

    // One instance: a second one hands its arguments to the first and ends.
    if (app.sendToRunningInstance())
        return 0;

    ui::WebView view({.resources =
                          [](std::string_view path) -> std::optional<ui::Resource> {
                              if (const auto file = helloFiles(path))
                                  return ui::Resource{std::string(*file), ui::mimeTypeFor(path)};
                              return std::nullopt;
                          },
                      .name = "hello"});
    view.expose("greet", [](const Value& arguments) -> Value {
        return "Hello, " + arguments[0].stringOr("you") + ", from " SYMBOLENGINE_APP_NAME " " SYMBOLENGINE_APP_VERSION;
    });

    ui::Window window({.title = SYMBOLENGINE_APP_NAME, .size = {520, 360}, .minimumSize = {320, 200}});
    window.setContent(view.nativeView());

    // The names the system asks the program to open go to the page: files from the Finder or Explorer, and whatever a
    // second instance was started with.
    const auto show = [&view](Array names) { view.send("opened", std::move(names)); };
    app.onOpenFiles = [&show](std::vector<std::filesystem::path> files) {
        Array names;
        for (const auto& file : files)
            names.emplace_back(paths::toUtf8(file.filename()));
        show(std::move(names));
    };
    app.onAnotherInstance = [&show](std::vector<std::string> arguments) {
        show(Array(arguments.begin(), arguments.end()));
    };
    app.onReopen = [&window] { window.toFront(); };

    // Closing the window, or the system asking, quits; a program with unsaved work would ask first.
    window.onCloseButton = [&app] { app.requestQuit(); };
    app.onQuitRequested = [&app] { app.quit(); };

    // On a Mac, the menu bar with the program's menu and Quit; on Windows this shows nothing.
    ui::MenuBar menus({{.title = "About " SYMBOLENGINE_APP_NAME, .chosen = [&view] { view.send("about", nullptr); }}},
                      {}, [&app] { app.requestQuit(); });

    window.setVisible(true);
    window.toFront();
    return app.run();
}
