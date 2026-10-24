#include <symbolengine/ui/Window.h>

#include <choc/platform/choc_ObjectiveCHelpers.h>

#import <AppKit/AppKit.h>
#include <objc/runtime.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace symbolengine::ui
{

namespace
{
// AppKit measures places up from the bottom of the main screen, and a Bounds down from its top.
CGFloat mainScreenHeight()
{
    NSArray<NSScreen*>* screens = [NSScreen screens];
    return screens.count > 0 ? screens[0].frame.size.height : 0.0;
}

NSRect toAppKit(const Bounds& b)
{
    return NSMakeRect(b.x, mainScreenHeight() - b.y - b.height, b.width, b.height);
}

Bounds fromAppKit(NSRect r)
{
    return {static_cast<int>(std::lround(r.origin.x)),
            static_cast<int>(std::lround(mainScreenHeight() - r.origin.y - r.size.height)),
            static_cast<int>(std::lround(r.size.width)), static_cast<int>(std::lround(r.size.height))};
}

NSColor* colourOf(std::uint32_t rgb)
{
    return [NSColor colorWithSRGBRed:((rgb >> 16) & 0xFF) / 255.0
                               green:((rgb >> 8) & 0xFF) / 255.0
                                blue:(rgb & 0xFF) / 255.0
                               alpha:1.0];
}

NSScreen* mainScreen()
{
    NSArray<NSScreen*>* screens = [NSScreen screens];
    return screens.count > 0 ? screens[0] : nil;
}

const char implKey = 0;
}

struct Window::Impl
{
    Window* owner = nullptr;
    NSWindow* window = nil;
    NSView* container = nil;
    id delegate = nil;
    std::vector<id> observers;
    Size minimum{};

    // Shared with the close button's call, which comes after the click and so maybe after the window has gone.
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);

    // The delegate's class, made at run time under a name of its own, so two programs' copies of SymbolEngine in one
    // process, as plugins in a host, never share one. It answers only the close button, which it leaves to the program.
    static Class delegateClass()
    {
        static Class made = [] {
            auto c = choc::objc::createDelegateClass("NSObject", "SymbolEngineWindowDelegate_");
            class_addMethod(c, @selector(windowShouldClose:),
                            (IMP) + [](id self, SEL, id) -> BOOL {
                                auto* impl = static_cast<Impl*>(
                                    [static_cast<NSValue*>(objc_getAssociatedObject(self, &implKey)) pointerValue]);
                                if (impl == nullptr)
                                    return NO;

                                std::weak_ptr<bool> alive = impl->alive;
                                Window* owner = impl->owner;
                                // A copy is called, as the program may destroy the window, and the function with it.
                                dispatch_async(dispatch_get_main_queue(), ^{
                                  if (alive.lock() == nullptr || !*alive.lock())
                                      return;
                                  if (auto pressed = owner->onCloseButton)
                                      pressed();
                                });
                                return NO;
                            },
                            "c@:@");
            objc_registerClassPair(c);
            return c;
        }();
        return made;
    }

    NSRect contentRect() const
    {
        return [window contentRectForFrameRect:window.frame];
    }

    // The screen a frame overlaps most, or the main screen when it overlaps none.
    static NSScreen* screenFor(NSRect frame)
    {
        NSScreen* best = nil;
        CGFloat most = 0.0;
        for (NSScreen* screen in [NSScreen screens])
        {
            const auto overlap = NSIntersectionRect(frame, screen.visibleFrame);
            if (overlap.size.width * overlap.size.height > most)
            {
                most = overlap.size.width * overlap.size.height;
                best = screen;
            }
        }
        return best != nil ? best : mainScreen();
    }

    // A frame held to the screen it overlaps most.
    static NSRect fitted(NSRect frame)
    {
        NSScreen* screen = screenFor(frame);
        if (screen == nil)
            return frame;

        const auto area = screen.visibleFrame;
        frame.size.width = std::min(frame.size.width, area.size.width);
        frame.size.height = std::min(frame.size.height, area.size.height);
        frame.origin.x = std::clamp(frame.origin.x, area.origin.x, NSMaxX(area) - frame.size.width);
        frame.origin.y = std::clamp(frame.origin.y, area.origin.y, NSMaxY(area) - frame.size.height);
        return frame;
    }

    Size held(Size size) const
    {
        return {std::max(size.width, minimum.width), std::max(size.height, minimum.height)};
    }
};

Window::Window(Options options) : impl(std::make_unique<Impl>())
{
    auto& w = *impl;
    w.owner = this;
    w.minimum = options.minimumSize;
    const auto size = w.held(options.size);

    NSWindowStyleMask style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable;
    if (options.resizable)
        style |= NSWindowStyleMaskResizable;

    w.window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, size.width, size.height)
                                           styleMask:style
                                             backing:NSBackingStoreBuffered
                                               defer:NO];
    w.window.releasedWhenClosed = NO;
    w.window.tabbingMode = NSWindowTabbingModeDisallowed;
    w.window.backgroundColor = colourOf(options.background);
    w.window.contentMinSize = NSMakeSize(w.minimum.width, w.minimum.height);
    [[w.window standardWindowButton:NSWindowZoomButton] setEnabled:NO];
    setTitle(options.title);

    w.container = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, size.width, size.height)];
    w.container.autoresizesSubviews = YES;
    w.window.contentView = w.container;

    w.delegate = [[Impl::delegateClass() alloc] init];
    objc_setAssociatedObject(w.delegate, &implKey, [NSValue valueWithPointer:impl.get()], OBJC_ASSOCIATION_RETAIN);
    w.window.delegate = w.delegate;

    auto* notifications = [NSNotificationCenter defaultCenter];
    const auto observe = [&](NSNotificationName name, void (^block)(NSNotification*)) {
        w.observers.push_back([[notifications addObserverForName:name object:w.window queue:nil usingBlock:block] retain]);
    };
    Window* owner = this;
    observe(NSWindowDidBecomeKeyNotification, ^(NSNotification*) {
      if (owner->onActiveChanged)
          owner->onActiveChanged(true);
    });
    observe(NSWindowDidResignKeyNotification, ^(NSNotification*) {
      if (owner->onActiveChanged)
          owner->onActiveChanged(false);
    });
    observe(NSWindowDidMoveNotification, ^(NSNotification*) {
      if (owner->onMovedOrResized)
          owner->onMovedOrResized();
    });
    observe(NSWindowDidResizeNotification, ^(NSNotification*) {
      if (owner->onMovedOrResized)
          owner->onMovedOrResized();
    });

    centre();
}

Window::~Window()
{
    auto& w = *impl;
    *w.alive = false;

    for (id observer : w.observers)
    {
        [[NSNotificationCenter defaultCenter] removeObserver:observer];
        [observer release];
    }

    w.window.delegate = nil;
    objc_setAssociatedObject(w.delegate, &implKey, nil, OBJC_ASSOCIATION_RETAIN);
    for (NSView* view in [[w.container.subviews copy] autorelease])
        [view removeFromSuperview];

    [w.window orderOut:nil];
    [w.window close];
    [w.window release];
    [w.container release];
    [w.delegate release];
}

void Window::setContent(void* view)
{
    auto& w = *impl;
    for (NSView* old in [[w.container.subviews copy] autorelease])
        [old removeFromSuperview];

    if (view == nullptr)
        return;

    NSView* content = (__bridge NSView*) view;
    content.frame = w.container.bounds;
    content.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [w.container addSubview:content];
}

void Window::setTitle(const std::string& title)
{
    NSString* text = [NSString stringWithUTF8String:title.c_str()];
    impl->window.title = text != nil ? text : @"";
}

void Window::setVisible(bool visible)
{
    if (visible)
        [impl->window orderFront:nil];
    else
        [impl->window orderOut:nil];
}

bool Window::isVisible() const
{
    return impl->window.visible || impl->window.miniaturized;
}

void Window::toFront()
{
    if (@available(macOS 14, *))
        [NSApp activate];
    else
        [NSApp activateIgnoringOtherApps:YES];

    if (impl->window.miniaturized)
        [impl->window deminiaturize:nil];
    [impl->window makeKeyAndOrderFront:nil];
}

bool Window::isActive() const
{
    return impl->window.keyWindow;
}

bool Window::isMinimised() const
{
    return impl->window.miniaturized;
}

void Window::setAlwaysOnTop(bool onTop)
{
    impl->window.level = onTop ? NSFloatingWindowLevel : NSNormalWindowLevel;
}

void Window::setSeeThrough(bool seeThrough)
{
    impl->window.alphaValue = seeThrough ? 0.0 : 1.0;
}

void Window::setAppearance(Appearance appearance)
{
    impl->window.appearance = appearance == Appearance::system ? nil
                              : [NSAppearance appearanceNamed:appearance == Appearance::dark ? NSAppearanceNameDarkAqua
                                                                                              : NSAppearanceNameAqua];
}

Size Window::size() const
{
    const auto content = impl->contentRect();
    return {static_cast<int>(std::lround(content.size.width)), static_cast<int>(std::lround(content.size.height))};
}

void Window::setSize(Size size)
{
    auto& w = *impl;
    size = w.held(size);
    const auto content = w.contentRect();
    const auto top = NSMaxY(content);
    const auto frame = [w.window frameRectForContentRect:NSMakeRect(content.origin.x, top - size.height, size.width, size.height)];
    [w.window setFrame:frame display:YES];
}

Size Window::workArea() const
{
    NSScreen* screen = impl->window.screen != nil ? impl->window.screen : mainScreen();
    const auto area = screen != nil ? screen.visibleFrame.size : NSMakeSize(1280, 800);
    return {static_cast<int>(area.width), static_cast<int>(area.height)};
}

Bounds Window::bounds() const
{
    return fromAppKit(impl->contentRect());
}

void Window::setBounds(Bounds bounds)
{
    auto& w = *impl;
    const auto size = w.held({bounds.width, bounds.height});
    bounds.width = size.width;
    bounds.height = size.height;
    const auto frame = Impl::fitted([w.window frameRectForContentRect:toAppKit(bounds)]);
    [w.window setFrame:frame display:YES];
}

void Window::centre()
{
    auto& w = *impl;
    NSScreen* screen = mainScreen();
    if (screen == nil)
        return;

    const auto area = screen.visibleFrame;
    auto frame = w.window.frame;
    frame.origin.x = area.origin.x + (area.size.width - frame.size.width) / 2;
    frame.origin.y = area.origin.y + (area.size.height - frame.size.height) / 2;
    [w.window setFrame:Impl::fitted(frame) display:YES];
}

void* Window::nativeHandle() const
{
    return (__bridge void*) impl->window;
}

}
