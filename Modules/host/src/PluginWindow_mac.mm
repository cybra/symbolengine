#include "PluginWindowNative.h"

#import <AppKit/AppKit.h>

namespace symbolengine::pluginWindowNative
{

void prepare()
{
    [NSApplication sharedApplication];
}

void* makeContainer(ui::Window& window)
{
    const auto size = window.size();
    NSView* container = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, size.width, size.height)];

    if (@available(macOS 14, *))
        container.clipsToBounds = YES;

    return container;
}

void releaseContainer(void* container)
{
    [static_cast<NSView*>(container) release];
}

// Some views settle their scale only once their parent's frame changes after they are put in it.
void settle(void* container, std::weak_ptr<bool> alive)
{
    NSView* view = static_cast<NSView*>(container);
    const auto size = view.frame.size;
    [view setFrameSize:NSMakeSize(size.width + 1, size.height + 1)];
    dispatch_async(dispatch_get_main_queue(), ^{
      if (const auto still = alive.lock(); still != nullptr && *still)
          view.frame = view.superview.bounds;
    });
}

// The awareness it would put back is Windows' alone.
Awareness::Awareness()
{
    static_cast<void>(previous);
}

Awareness::~Awareness() = default;

}
