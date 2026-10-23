#pragma once

#include <symbolengine/plugin/Plugin.h>
#include <symbolengine/ui/Native.h>
#include <symbolengine/ui/WebView.h>

#include "Changes.h"

#include <functional>
#include <memory>
#include <vector>

namespace symbolengine::plugin::detail
{

/** A plugin's editor open in a host: its page in a web view, the functions plugin.js calls for the parameters, and a
    timer that sends the page each value that changes and lets the plugin send what it measures. Message thread only. */
class EditorSession final : public EditorLink
{
public:
    /** The page is named after the plugin's id, so its stored data is the plugin's own. told is called after each
        change the page makes, so the host can be asked to collect them if the plugin isn't processing. */
    EditorSession(Plugin& plugin, const Description& description, Editor editor, EditorChanges& changes,
                  std::function<void()> told);
    ~EditorSession() override;

    EditorSession(const EditorSession&) = delete;
    EditorSession& operator=(const EditorSession&) = delete;

    bool available() const noexcept { return view.available(); }
    void* nativeView() const noexcept { return view.nativeView(); }
    const Editor& description() const noexcept { return editor; }

    void expose(std::string name, std::function<Value(const Value& arguments)> function) override;
    void send(std::string_view event, const Value& data) override;

private:
    Plugin& plugin;
    Editor editor;
    EditorChanges& changes;
    std::function<void()> told;
    ui::WebView view;
    std::vector<double> sent;   // each parameter's value as the page last had it
    std::unique_ptr<ui::Timer> timer;

    std::size_t indexOf(const Value& id) const;
    void change(EditorChange::Kind kind, std::uint32_t id, double value);
    void tick();
};

}
