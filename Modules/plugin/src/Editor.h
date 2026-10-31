#pragma once

#include <symbolengine/plugin/Plugin.h>
#include <symbolengine/ui/Native.h>
#include <symbolengine/ui/WebView.h>

#include "Changes.h"

#include <functional>
#include <memory>
#include <set>
#include <utility>
#include <vector>

namespace symbolengine::plugin::detail
{

/** A plugin's editor open in a host: its page in a web view, the functions plugin.js calls for the parameters, its
    size, and a timer that sends the page each value that changes, lets the plugin send what it measures, and keeps
    the size the user gives it. Sizes here are in points. Message thread only. */
class EditorSession final : public EditorLink
{
public:
    /** How the session reaches the host: told is called after each change the page makes, so the host can be asked
        to collect them if the plugin isn't processing, and resize asks the host for a new size in points, saying
        whether it agreed. */
    struct Host
    {
        std::function<void()> told;
        std::function<bool(std::uint32_t width, std::uint32_t height)> resize;
        double pixelsPerPoint = 1.0;
    };

    /** The page is named after the plugin's id, so its stored data is the plugin's own. */
    EditorSession(Plugin& plugin, const Description& description, Editor editor, EditorChanges& changes, Host host);
    ~EditorSession() override;

    EditorSession(const EditorSession&) = delete;
    EditorSession& operator=(const EditorSession&) = delete;

    bool available() const noexcept { return view.available(); }
    void* nativeView() const noexcept { return view.nativeView(); }
    const Editor& description() const noexcept { return editor; }

    std::uint32_t width() const noexcept { return size.first; }
    std::uint32_t height() const noexcept { return size.second; }

    /** A size held to the editor's limits. */
    std::pair<std::uint32_t, std::uint32_t> heldToLimits(std::uint32_t width, std::uint32_t height) const noexcept;

    /** The editor has been resized, by the host or after the host agreed to the page's or the plugin's request. */
    void resizedTo(std::uint32_t width, std::uint32_t height);

    /** The host has said how many pixels a point is, as it may only after the editor is made: the opening size is worked
        out again for it. */
    void fitToScale(double pixelsPerPoint);

    void expose(std::string name, std::function<Value(const Value& arguments)> function) override;
    void send(std::string_view event, const Value& data) override;
    void snapshot(const std::filesystem::path& png, std::function<void(bool written)> done) override;
    void beginGesture(std::uint32_t id) override;
    double setParameter(std::uint32_t id, double value) override;
    void endGesture(std::uint32_t id) override;

private:
    Plugin& plugin;
    Editor editor;
    EditorChanges& changes;
    Host host;
    ui::WebView view;
    std::vector<double> sent;   // each parameter's value as the page last had it
    std::set<std::uint32_t> openGestures;   // the parameters with a gesture begun and not yet ended
    std::pair<std::uint32_t, std::uint32_t> size;
    std::pair<std::uint32_t, std::uint32_t> kept;   // the size last kept in the size file
    std::uint32_t tickMilliseconds;
    int restingTicks = 0;
    int ticks = 0;
    std::filesystem::path file;   // where the size is kept for new editors
    std::unique_ptr<ui::Timer> timer;

    std::filesystem::path sizeFile(const Description& description) const;
    std::pair<std::uint32_t, std::uint32_t> openingSize() const;
    std::size_t indexOf(const Value& id) const;
    std::size_t indexOf(std::uint32_t id) const;
    void record(EditorChange::Kind kind, std::uint32_t id, double value);
    void keepSize(bool now);
    void snapshotWhenAsked();
    void tick();
};

}
