// The test bundle's question on a Mac: a window of its own, shown as a license question is, for as long as asked: a
// normal window with the run loop turning meanwhile, as a plugin waiting for an answer does, or a modal one in its own
// modal session. It is a real window, ordered in and seen by the system, placed far off every display, where macOS
// leaves a window without a title bar, so it never covers what the person running the tests is doing.

#import <AppKit/AppKit.h>

// A window that stays where it was put, as a modal session would otherwise center it on the display.
@interface SymbolEngineOffscreenQuestion : NSWindow
@end

@implementation SymbolEngineOffscreenQuestion
- (void)center
{
}
@end

extern "C" void askSomething(int milliseconds, bool modal)
{
    @autoreleasepool
    {
        NSWindow* window = [[SymbolEngineOffscreenQuestion alloc] initWithContentRect:NSMakeRect(-30000, -30000, 320, 160)
                                                       styleMask:NSWindowStyleMaskBorderless
                                                         backing:NSBackingStoreBuffered
                                                           defer:NO];
        window.releasedWhenClosed = NO;
        if (modal)
        {
            NSTimer* done = [NSTimer timerWithTimeInterval:milliseconds / 1000.0
                                                   repeats:NO
                                                     block:^(NSTimer*) { [NSApp stopModal]; }];
            [[NSRunLoop currentRunLoop] addTimer:done forMode:NSModalPanelRunLoopMode];
            [NSApp runModalForWindow:window];
        }
        else
        {
            [window orderFront:nil];
            [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:milliseconds / 1000.0]];
        }
        [window close];
    }
}
