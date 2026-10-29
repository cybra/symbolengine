// The editor test plugin's view on a Mac: an NSView of its own in the host's, with an off-screen view beside it, as
// plugins that draw off screen keep, and the host's view watched for the sizes it is given.

#include "TestEditor.h"

#include "pluginterfaces/gui/iplugview.h"

#import <AppKit/AppKit.h>

#include <cstring>
#include <vector>

namespace
{

constexpr NSRect offscreenFrame{{-1000, -1000}, {10, 10}};

struct Native
{
    NSView* parent = nil;
    NSView* own = nil;
    NSView* offscreen = nil;
    id watcher = nil;
    std::vector<NSSize> parentSizes;
};

}

namespace testEditor
{

bool supported(const char* platformType)
{
    return std::strcmp(platformType, Steinberg::kPlatformTypeNSView) == 0;
}

void* attach(void* parent, int width, int height)
{
    auto* native = new Native;
    native->parent = static_cast<NSView*>(parent);
    native->own = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, width, height)];
    native->offscreen = [[NSView alloc] initWithFrame:offscreenFrame];
    [native->parent addSubview:native->own];
    [native->parent addSubview:native->offscreen];

    native->parent.postsFrameChangedNotifications = YES;
    native->watcher =
        [[[NSNotificationCenter defaultCenter] addObserverForName:NSViewFrameDidChangeNotification
                                                           object:native->parent
                                                            queue:nil
                                                       usingBlock:^(NSNotification*) {
                                                         native->parentSizes.push_back(native->parent.frame.size);
                                                       }] retain];
    return native;
}

void resize(void* view, int width, int height)
{
    auto* native = static_cast<Native*>(view);
    [native->own setFrameSize:NSMakeSize(width, height)];
}

void detach(void* view)
{
    auto* native = static_cast<Native*>(view);
    [[NSNotificationCenter defaultCenter] removeObserver:native->watcher];
    [native->watcher release];
    [native->offscreen removeFromSuperview];
    [native->own removeFromSuperview];
    [native->offscreen release];
    [native->own release];
    delete native;
}

bool stillWhole(void* parent, void* view)
{
    auto* native = static_cast<Native*>(view);
    auto* host = static_cast<NSView*>(parent);
    return host == native->parent && host.window != nil && !host.hiddenOrHasHiddenAncestor && !native->own.hidden &&
           native->own.superview == host && native->offscreen.superview == host;
}

bool laidOut(void* parent)
{
    auto* host = static_cast<NSView*>(parent);
    bool clips = true;

    if (@available(macOS 14, *))
        clips = host.clipsToBounds;

    return host.window != nil && !host.flipped && clips;
}

bool sized(void* parent, int width, int height)
{
    const auto size = static_cast<NSView*>(parent).frame.size;
    return size.width == width && size.height == height;
}

void ownSize(void* view, int& width, int& height)
{
    const auto size = static_cast<Native*>(view)->own.frame.size;
    width = static_cast<int>(size.width);
    height = static_cast<int>(size.height);
}

bool awareAsParent(void*)
{
    return true;
}

bool takesDrops(void*)
{
    return true;
}

bool offscreenMoved(void* view)
{
    auto* native = static_cast<Native*>(view);
    return !NSEqualRects(native->offscreen.frame, offscreenFrame);
}

int nudges(void* view)
{
    const auto& sizes = static_cast<Native*>(view)->parentSizes;
    int count = 0;

    for (std::size_t i = 0; i + 1 < sizes.size(); ++i)
        if (sizes[i].width == sizes[i + 1].width + 1 && sizes[i].height == sizes[i + 1].height + 1)
            ++count;

    return count;
}

}
