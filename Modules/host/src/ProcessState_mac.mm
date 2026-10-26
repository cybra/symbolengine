#include "ProcessState.h"

#import <AppKit/AppKit.h>

namespace symbolengine
{

bool mouseCoalescing()
{
    return [NSEvent isMouseCoalescingEnabled];
}

void setMouseCoalescing(bool on)
{
    [NSEvent setMouseCoalescingEnabled:on];
}

}
