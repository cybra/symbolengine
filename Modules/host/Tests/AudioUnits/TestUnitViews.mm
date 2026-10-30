// The test units' Cocoa views, each made by a factory class of its own, as an Audio Unit's view is: every one checks
// that its unit is initialized whenever it is made, put in a view, resized or taken out, and that it is never hidden
// before it is taken out or moved to another view; and most misbehave one way, as units' views do. What was wrong goes
// to the units' own log.

#import <AppKit/AppKit.h>
#import <AudioToolbox/AUCocoaUIView.h>

#include <AudioToolbox/AudioToolbox.h>

#include <string>

extern "C" void symbolEngineTestUnitMisuse(const char* unit, const char* what);

namespace
{

constexpr AudioUnitPropertyID initializedProperty = 64100;

enum Kind
{
    fixed,
    sizable,
    grows,
    zero,
    origin
};

const char* const names[] = {"ViewFixed", "ViewSizable", "ViewGrows", "ViewZero", "ViewOrigin"};

bool initialized(AudioUnit unit)
{
    UInt32 answer = 0, size = sizeof answer;
    return AudioUnitGetProperty(unit, initializedProperty, kAudioUnitScope_Global, 0, &answer, &size) == noErr &&
           answer != 0;
}

}

@interface SymbolEngineTestUnitView : NSView
- (instancetype)initWithUnit:(AudioUnit)unit kind:(int)kind;
@end

@implementation SymbolEngineTestUnitView
{
    AudioUnit unit;
    int kind;
    NSTimer* timer;
    NSView* firstParent;
}

- (instancetype)initWithUnit:(AudioUnit)given kind:(int)made
{
    // One that has no size until after it is shown starts at none.
    self = [super initWithFrame:made == zero ? NSZeroRect : NSMakeRect(0, 0, 500, 300)];
    unit = given;
    kind = made;

    if (kind == sizable)
        self.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;

    [self check:"made"];
    return self;
}

- (void)check:(const char*)what
{
    if (!initialized(unit))
        symbolEngineTestUnitMisuse(names[kind],
                                   ("its view " + std::string(what) + " while the unit wasn't initialized").c_str());
}

- (void)viewWillMoveToSuperview:(NSView*)next
{
    [self check:next == nil ? "taken out" : "put in a view"];

    if (next == nil)
    {
        if (self.hiddenOrHasHiddenAncestor)
            symbolEngineTestUnitMisuse(names[kind], "its view hidden before it was taken out");

        [timer invalidate];
        timer = nil;
    }
    else if (firstParent != nil && next != firstParent)
        symbolEngineTestUnitMisuse(names[kind], "its view moved to another view");

    [super viewWillMoveToSuperview:next];
}

- (void)viewDidMoveToSuperview
{
    [super viewDidMoveToSuperview];

    if (self.superview == nil || firstParent != nil)
        return;

    firstParent = self.superview;

    // Grows itself ten points each way five times, a tenth of a second apart.
    if (kind == grows)
    {
        __block int times = 0;
        timer = [NSTimer scheduledTimerWithTimeInterval:0.1
                                                repeats:YES
                                                  block:^(NSTimer* running) {
                                                    const auto size = self.frame.size;
                                                    [self setFrameSize:NSMakeSize(size.width + 10, size.height + 10)];
                                                    if (++times == 5)
                                                        [running invalidate];
                                                  }];
    }

    // Says its size only once it has been shown.
    if (kind == zero)
        dispatch_async(dispatch_get_main_queue(), ^{
          if (self.superview != nil)
              [self setFrameSize:NSMakeSize(600, 400)];
        });

    // One that moves its origin as it is resized resizes itself once, a fifth of a second after it is shown.
    if (kind == origin)
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 200 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
          if (self.superview != nil)
              [self setFrameSize:NSMakeSize(520, 320)];
        });
}

- (void)setFrameSize:(NSSize)size
{
    [self check:"resized"];
    [super setFrameSize:size];

    // Moves its own origin as it is resized.
    if (kind == origin)
        [super setFrameOrigin:NSMakePoint(10, 10)];
}

@end

@interface SymbolEngineTestUnitViewFixed : NSObject <AUCocoaUIBase>
@end

@implementation SymbolEngineTestUnitViewFixed
- (unsigned)interfaceVersion
{
    return 0;
}

- (NSView*)uiViewForAudioUnit:(AudioUnit)unit withSize:(NSSize)size
{
    return [[[SymbolEngineTestUnitView alloc] initWithUnit:unit kind:fixed] autorelease];
}
@end

@interface SymbolEngineTestUnitViewSizable : NSObject <AUCocoaUIBase>
@end

@implementation SymbolEngineTestUnitViewSizable
- (unsigned)interfaceVersion
{
    return 0;
}

- (NSView*)uiViewForAudioUnit:(AudioUnit)unit withSize:(NSSize)size
{
    return [[[SymbolEngineTestUnitView alloc] initWithUnit:unit kind:sizable] autorelease];
}
@end

@interface SymbolEngineTestUnitViewGrows : NSObject <AUCocoaUIBase>
@end

@implementation SymbolEngineTestUnitViewGrows
- (unsigned)interfaceVersion
{
    return 0;
}

- (NSView*)uiViewForAudioUnit:(AudioUnit)unit withSize:(NSSize)size
{
    return [[[SymbolEngineTestUnitView alloc] initWithUnit:unit kind:grows] autorelease];
}
@end

@interface SymbolEngineTestUnitViewZero : NSObject <AUCocoaUIBase>
@end

@implementation SymbolEngineTestUnitViewZero
- (unsigned)interfaceVersion
{
    return 0;
}

- (NSView*)uiViewForAudioUnit:(AudioUnit)unit withSize:(NSSize)size
{
    return [[[SymbolEngineTestUnitView alloc] initWithUnit:unit kind:zero] autorelease];
}
@end

@interface SymbolEngineTestUnitViewOrigin : NSObject <AUCocoaUIBase>
@end

@implementation SymbolEngineTestUnitViewOrigin
- (unsigned)interfaceVersion
{
    return 0;
}

- (NSView*)uiViewForAudioUnit:(AudioUnit)unit withSize:(NSSize)size
{
    return [[[SymbolEngineTestUnitView alloc] initWithUnit:unit kind:origin] autorelease];
}
@end

// Throws unless its unit is initialized, as a view that builds itself from what initializing made.
@interface SymbolEngineTestUnitViewNeedsInit : NSObject <AUCocoaUIBase>
@end

@implementation SymbolEngineTestUnitViewNeedsInit
- (unsigned)interfaceVersion
{
    return 0;
}

- (NSView*)uiViewForAudioUnit:(AudioUnit)unit withSize:(NSSize)size
{
    if (!initialized(unit))
        [NSException raise:@"SymbolEngineTestUnitView" format:@"its unit isn't initialized"];

    return [[[SymbolEngineTestUnitView alloc] initWithUnit:unit kind:fixed] autorelease];
}
@end

// Always throws.
@interface SymbolEngineTestUnitViewThrows : NSObject <AUCocoaUIBase>
@end

@implementation SymbolEngineTestUnitViewThrows
- (unsigned)interfaceVersion
{
    return 0;
}

- (NSView*)uiViewForAudioUnit:(AudioUnit)unit withSize:(NSSize)size
{
    [NSException raise:@"SymbolEngineTestUnitView" format:@"the test view's own exception"];
    return nil;
}
@end
