#include "PluginWindowNative.h"
#include "PluginWindowsNative.h"

#import <AppKit/AppKit.h>
#include <objc/runtime.h>

#include <cstdint>
#include <string>

namespace symbolengine::pluginWindowNative
{

namespace
{

const char handedUpKey = 0;

using HandedUp = std::function<void(char32_t key, bool repeat)>;

HandedUp* handedUpOf(id view)
{
    return static_cast<HandedUp*>([static_cast<NSValue*>(objc_getAssociatedObject(view, &handedUpKey)) pointerValue]);
}

// The container's class, made at run time under a name of its own, so two copies of SymbolEngine in one process, as
// plugins in a host, never share one: a view that takes the keys a plugin's view hands up to it quietly, where an
// NSView would pass them on to be refused with a beep, telling of each pressed with no modifier key while no text is
// being typed.
Class containerClass()
{
    static Class made = [] {
        const auto name = "SymbolEngineEditorView_" + std::to_string(reinterpret_cast<std::uintptr_t>(&handedUpKey));
        auto made = objc_allocateClassPair([NSView class], name.c_str(), 0);
        class_addMethod(
            made, @selector(keyDown:),
            (IMP) +
                [](id self, SEL, NSEvent* event) {
                    constexpr auto modifiers = NSEventModifierFlagCommand | NSEventModifierFlagControl |
                                               NSEventModifierFlagOption | NSEventModifierFlagShift;
                    NSString* characters = event.charactersIgnoringModifiers;
                    auto* handedUp = handedUpOf(self);

                    if (handedUp == nullptr || characters.length == 0 || (event.modifierFlags & modifiers) != 0 ||
                        pluginWindowsNative::typingIn(static_cast<NSView*>(self).window))
                        return;

                    char32_t key = 0;
                    [characters getBytes:&key
                               maxLength:sizeof key
                              usedLength:nullptr
                                encoding:NSUTF32LittleEndianStringEncoding
                                 options:0
                                   range:NSMakeRange(0, [characters rangeOfComposedCharacterSequenceAtIndex:0].length)
                          remainingRange:nullptr];
                    (*handedUp)(key, event.isARepeat);
                },
            "v@:@");
        objc_registerClassPair(made);
        return made;
    }();
    return made;
}

}

void prepare()
{
    [NSApplication sharedApplication];
}

void* makeContainer(ui::Window& window, std::function<void(char32_t key, bool repeat)> handedUp)
{
    const auto size = window.size();
    NSView* container = [[containerClass() alloc] initWithFrame:NSMakeRect(0, 0, size.width, size.height)];
    objc_setAssociatedObject(container, &handedUpKey, [NSValue valueWithPointer:new HandedUp(std::move(handedUp))],
                             OBJC_ASSOCIATION_RETAIN);

    if (@available(macOS 14, *))
        container.clipsToBounds = YES;

    return container;
}

void releaseContainer(void* container)
{
    NSView* view = static_cast<NSView*>(container);
    delete handedUpOf(view);
    objc_setAssociatedObject(view, &handedUpKey, nil, OBJC_ASSOCIATION_RETAIN);
    [view release];
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
