#include "symbolengine/host/PluginWindow.h"

#import <AppKit/AppKit.h>

#include <cmath>

namespace symbolengine
{

namespace
{
constexpr ui::Size smallest{100, 50};
}

// The window's own view the editor is put in, made with the window and kept for its life: not flipped, clipping to its
// bounds, never hidden, filling the window.
struct PluginWindow::Impl final : PluginEditorFrame
{
    PluginWindow* owner = nullptr;
    std::shared_ptr<Plugin> plugin;
    std::unique_ptr<ui::Window> window;
    NSView* container = nil;
    std::unique_ptr<PluginEditor> editor;
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);

    ~Impl() override
    {
        // The editor goes while the window and its view are whole, then the window.
        *alive = false;
        editor.reset();
        window.reset();
        [container release];
    }

    void place(PluginEditorSize size) override { window->setSize({size.width, size.height}); }

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

    // Some views settle their scale only once their parent's frame changes after they are put in it, so it is made a
    // point larger each way and put back on the next turn of the run loop.
    void settle()
    {
        NSView* view = container;
        const auto size = view.frame.size;
        [view setFrameSize:NSMakeSize(size.width + 1, size.height + 1)];
        std::weak_ptr<bool> living = alive;
        dispatch_async(dispatch_get_main_queue(), ^{
          if (const auto still = living.lock(); still != nullptr && *still)
              view.frame = view.superview.bounds;
        });
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

    [NSApplication sharedApplication];
    auto impl = std::make_unique<Impl>();
    impl->plugin = plugin;
    impl->window = std::make_unique<ui::Window>(
        ui::Window::Options{options.title, {400, 300}, smallest, true, false, options.background});

    impl->container = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 400, 300)];
    if (@available(macOS 14, *))
        impl->container.clipsToBounds = YES;
    impl->window->setContent(impl->container);

    impl->editor = PluginEditor::open(std::move(plugin), impl->container, *impl, error);

    if (impl->editor == nullptr)
        return nullptr;

    impl->window->setResizable(impl->editor->resizable());
    impl->window->center();
    impl->settle();

    std::unique_ptr<PluginWindow> made(new PluginWindow(std::move(impl)));
    auto* state = made->impl.get();
    state->owner = made.get();
    auto* window = made.get();
    state->window->onMovedOrResized = [state] { state->movedOrResized(); };
    state->window->onResizing = [state](ui::Size wanted) { return state->resizing(wanted); };
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
    const auto size = impl->window->size();
    impl->window->setBounds({x, y, size.width, size.height});
}

void PluginWindow::center()
{
    impl->window->center();
}

void* PluginWindow::nativeHandle() const
{
    return impl->window->nativeHandle();
}

}
