#include "symbolengine/host/PluginWindow.h"

#include "PluginWindowNative.h"

namespace symbolengine
{

namespace
{
constexpr ui::Size smallest{100, 50};
}

// The window and the view in it the editor is put in, made with the window and kept for its life. Sizes are the
// editor's own: points on a Mac and pixels on Windows, the window measuring itself in pixels there.
struct PluginWindow::Impl final : PluginEditorFrame
{
    PluginWindow* owner = nullptr;
    std::shared_ptr<Plugin> plugin;
    std::unique_ptr<ui::Window> window;
    void* container = nullptr;
    std::unique_ptr<PluginEditor> editor;
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);

    ~Impl() override
    {
        // The editor goes while the window and its view are whole, then the window, then the view.
        *alive = false;
        editor.reset();

        const pluginWindowNative::Awareness awareness;
        window.reset();

        if (container != nullptr)
            pluginWindowNative::releaseContainer(container);
    }

    void place(PluginEditorSize size) override
    {
        const pluginWindowNative::Awareness awareness;
        window->setSize({size.width, size.height});
    }

    double scale() const override { return window->scale(); }

    // The window resized by the user, or by the program, told to the editor when it isn't the editor's own size.
    void movedOrResized()
    {
        if (editor != nullptr)
            if (const auto now = window->size(); PluginEditorSize{now.width, now.height} != editor->size())
                editor->resized({now.width, now.height});

        if (owner != nullptr && owner->onMovedOrResized)
            owner->onMovedOrResized();
    }

    // A drag of its edge held to the nearest size the editor takes, the size it has for one that can't be resized.
    ui::Size resizing(ui::Size wanted) const
    {
        if (editor == nullptr)
            return window->size();

        const auto taken = editor->constrain({wanted.width, wanted.height});
        return {taken.width, taken.height};
    }

    void rescaled()
    {
        if (editor != nullptr)
            editor->rescaled(window->scale());
    }
};

std::unique_ptr<PluginWindow> PluginWindow::open(std::shared_ptr<Plugin> plugin, Options options, PluginError& error)
{
    error = {};

    if (plugin == nullptr)
    {
        error = {"There is no plugin.", "openEditor", 0};
        return nullptr;
    }

    pluginWindowNative::prepare();
    auto parts = std::make_unique<Impl>();
    parts->plugin = plugin;

    {
        const pluginWindowNative::Awareness awareness;
        parts->window = std::make_unique<ui::Window>(
            ui::Window::Options{options.title, {400, 300}, smallest, true, false, options.background, true});
        parts->container = pluginWindowNative::makeContainer(*parts->window);
    }

    if (parts->container == nullptr)
    {
        error = {"The window's view for its editor couldn't be made.", "openEditor", 0};
        return nullptr;
    }

    parts->window->setContent(parts->container);
    parts->editor = PluginEditor::open(std::move(plugin), parts->container, *parts, error);

    if (parts->editor == nullptr)
        return nullptr;

    {
        const pluginWindowNative::Awareness awareness;
        parts->window->setResizable(parts->editor->resizable());
        parts->window->center();
    }

    pluginWindowNative::settle(parts->container, parts->alive);

    std::unique_ptr<PluginWindow> made(new PluginWindow(std::move(parts)));
    auto* state = made->impl.get();
    state->owner = made.get();
    auto* window = made.get();
    state->window->onMovedOrResized = [state] { state->movedOrResized(); };
    state->window->onResizing = [state](ui::Size wanted) { return state->resizing(wanted); };
    state->window->onScaleChanged = [state] { state->rescaled(); };
    state->window->onCloseButton = [window] {
        if (auto pressed = window->onCloseButton)
            pressed();
    };
    state->window->onActiveChanged = [window](bool active) {
        if (window->onActiveChanged)
            window->onActiveChanged(active);
    };
    return made;
}

PluginWindow::PluginWindow(std::unique_ptr<Impl> made) : impl(std::move(made)) {}

PluginWindow::~PluginWindow()
{
    std::shared_ptr<Impl> closing(std::move(impl));
    closing->owner = nullptr;
    closing->window->onCloseButton = nullptr;
    closing->window->onActiveChanged = nullptr;

    // During a call into the plugin, as one its own dialog lets run: hidden now, and closed once the call returns.
    if (closing->plugin->isBusy())
    {
        closing->window->setVisible(false);
        const auto plugin = closing->plugin;
        plugin->whenFree([closing] {});
    }
}

const std::shared_ptr<Plugin>& PluginWindow::plugin() const
{
    return impl->plugin;
}

PluginEditorSize PluginWindow::editorSize() const
{
    return impl->editor->size();
}

bool PluginWindow::isResizable() const
{
    return impl->window->isResizable();
}

void PluginWindow::setTitle(const std::string& title)
{
    impl->window->setTitle(title);
}

void PluginWindow::setVisible(bool visible)
{
    impl->window->setVisible(visible);
}

bool PluginWindow::isVisible() const
{
    return impl->window->isVisible();
}

void PluginWindow::toFront()
{
    impl->window->toFront();
}

bool PluginWindow::isActive() const
{
    return impl->window->isActive();
}

void PluginWindow::setAlwaysOnTop(bool onTop)
{
    impl->window->setAlwaysOnTop(onTop);
}

ui::Bounds PluginWindow::bounds() const
{
    return impl->window->bounds();
}

void PluginWindow::moveTo(int x, int y)
{
    const pluginWindowNative::Awareness awareness;
    const auto size = impl->window->size();
    impl->window->setBounds({x, y, size.width, size.height});
}

void PluginWindow::center()
{
    const pluginWindowNative::Awareness awareness;
    impl->window->center();
}

void* PluginWindow::nativeHandle() const
{
    return impl->window->nativeHandle();
}

}
