#include "Vst3View.h"

#import <AppKit/AppKit.h>

namespace symbolengine::vst3View
{

void sizeOwnView(void* parent, int width, int height)
{
    NSView* host = static_cast<NSView*>(parent);
    NSView* own = host.subviews.firstObject;

    if (own == nil)
        return;

    // The host's view isn't flipped, so its top left is its height up.
    const auto frame = NSMakeRect(0, NSHeight(host.bounds) - height, width, height);

    if (!NSEqualRects(own.frame, frame))
        own.frame = frame;
}

// The awareness it would put back is Windows' alone.
ParentAwareness::ParentAwareness(void*)
{
    static_cast<void>(previous);
}

ParentAwareness::~ParentAwareness() = default;

}
