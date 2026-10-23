#include <symbolengine/ui/Native.h>

#import <AppKit/AppKit.h>

namespace symbolengine::ui::native
{

bool attach(void* view, void* parent)
{
    if (view == nullptr || parent == nullptr)
        return false;

    NSView* child = (__bridge NSView*) view;
    NSView* host = (__bridge NSView*) parent;
    [host addSubview:child];

    // A host's view may not be flipped, so the top left is found from its height.
    const auto size = child.frame.size;
    [child setFrameOrigin:NSMakePoint(0, host.isFlipped ? 0 : host.bounds.size.height - size.height)];
    return true;
}

void detach(void* view)
{
    if (view != nullptr)
        [(__bridge NSView*) view removeFromSuperview];
}

void resize(void* view, std::uint32_t width, std::uint32_t height)
{
    if (view == nullptr)
        return;

    NSView* child = (__bridge NSView*) view;
    [child setFrameSize:NSMakeSize(width, height)];

    if (NSView* host = child.superview; host != nil && !host.isFlipped)
        [child setFrameOrigin:NSMakePoint(0, host.bounds.size.height - height)];
}

void show(void* view, bool visible)
{
    if (view != nullptr)
        [(__bridge NSView*) view setHidden:!visible];
}

}
