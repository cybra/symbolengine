#include <symbolengine/ui/MenuBar.h>

#include <choc/platform/choc_ObjectiveCHelpers.h>

#import <AppKit/AppKit.h>
#include <objc/runtime.h>

#include <map>
#include <utility>

namespace symbolengine::ui
{

namespace
{
NSString* text(const std::string& value)
{
    NSString* converted = [NSString stringWithUTF8String:value.c_str()];
    return converted != nil ? converted : @"";
}

NSString* programName()
{
    id name = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"CFBundleName"];
    return [name isKindOfClass:[NSString class]] ? (NSString*) name : [[NSProcessInfo processInfo] processName];
}

const char implKey = 0;
}

struct MenuBar::Impl
{
    std::vector<MenuItem> appItems;
    std::vector<Menu> menus;
    std::function<void()> quit;

    id target = nil;   // takes every item's choice, and each menu's asking for its items as it opens
    NSMenu* bar = nil;
    NSMenu* appMenu = nil;
    std::vector<NSMenu*> shown;   // each menu given, in order

    // Each item's choice by the tag it is given, and the tags each menu gave out, the app menu's under -1.
    std::map<NSInteger, std::function<void()>> actions;
    std::map<int, std::vector<NSInteger>> tagsOf;
    NSInteger nextTag = 1;

    bool tracking = false, changedWhileTracking = false;
    std::vector<id> observers;

    static Class targetClass()
    {
        static Class made = [] {
            auto c = choc::objc::createDelegateClass("NSObject", "SymbolEngineMenuTarget_");
            class_addMethod(c, @selector(chosen:), (IMP) + [](id self, SEL, NSMenuItem* item) {
                if (auto* impl = owner(self))
                    impl->choose(item.tag);
            },
                            "v@:@");
            class_addMethod(c, @selector(menuNeedsUpdate:), (IMP) + [](id self, SEL, NSMenu* menu) {
                if (auto* impl = owner(self))
                    impl->refresh(menu);
            },
                            "v@:@");
            if (auto* protocol = objc_getProtocol("NSMenuDelegate"))
                class_addProtocol(c, protocol);
            objc_registerClassPair(c);
            return c;
        }();
        return made;
    }

    static Impl* owner(id target)
    {
        return static_cast<Impl*>([static_cast<NSValue*>(objc_getAssociatedObject(target, &implKey)) pointerValue]);
    }

    // A copy is called, as the choice may change the menus, and the function with them.
    void choose(NSInteger tag)
    {
        if (const auto found = actions.find(tag); found != actions.end())
            if (auto call = found->second)
                call();
    }

    void fill(NSMenu* menu, const std::vector<MenuItem>& items, std::vector<NSInteger>& tags)
    {
        [menu removeAllItems];
        menu.autoenablesItems = NO;

        for (const auto& item : items)
        {
            if (item.title.empty() && item.submenu.empty())
            {
                [menu addItem:[NSMenuItem separatorItem]];
                continue;
            }

            NSMenuItem* entry = [[NSMenuItem alloc] initWithTitle:text(item.title) action:nil keyEquivalent:@""];
            entry.enabled = item.enabled;
            entry.state = item.ticked ? NSControlStateValueOn : NSControlStateValueOff;

            if (!item.submenu.empty())
            {
                NSMenu* submenu = [[NSMenu alloc] initWithTitle:text(item.title)];
                fill(submenu, item.submenu, tags);
                entry.submenu = submenu;
                [submenu release];
            }
            else
            {
                const auto tag = nextTag++;
                actions[tag] = item.chosen;
                tags.push_back(tag);
                entry.tag = tag;
                entry.target = target;
                entry.action = @selector(chosen:);

                if (item.shortcut.has_value() && item.shortcut->key != 0)
                {
                    entry.keyEquivalent = [NSString stringWithFormat:@"%c", item.shortcut->key];
                    NSEventModifierFlags flags = NSEventModifierFlagCommand;
                    if (item.shortcut->shift)
                        flags |= NSEventModifierFlagShift;
                    if (item.shortcut->option)
                        flags |= NSEventModifierFlagOption;
                    entry.keyEquivalentModifierMask = flags;
                }
            }

            [menu addItem:entry];
            [entry release];
        }
    }

    // A menu's items asked for again, those it had let go of.
    void fillMenu(int index)
    {
        auto& tags = tagsOf[index];
        for (const auto tag : tags)
            actions.erase(tag);
        tags.clear();

        if (index < 0)
        {
            fill(appMenu, appItems, tags);
            addSystemItems();
        }
        else
        {
            const auto& menu = menus[static_cast<std::size_t>(index)];
            fill(shown[static_cast<std::size_t>(index)], menu.items ? menu.items() : std::vector<MenuItem>{}, tags);
        }
    }

    void refresh(NSMenu* menu)
    {
        for (std::size_t i = 0; i < shown.size(); ++i)
            if (shown[i] == menu)
                fillMenu(static_cast<int>(i));
    }

    // What every Mac app's own menu has after the program's items: Services, Hide, Hide Others, Show All and Quit.
    void addSystemItems()
    {
        NSString* name = programName();
        if (appMenu.numberOfItems > 0)
            [appMenu addItem:[NSMenuItem separatorItem]];

        NSMenuItem* services = [[NSMenuItem alloc] initWithTitle:@"Services" action:nil keyEquivalent:@""];
        NSMenu* servicesMenu = [[NSMenu alloc] initWithTitle:@"Services"];
        services.submenu = servicesMenu;
        [NSApp setServicesMenu:servicesMenu];
        [appMenu addItem:services];
        [servicesMenu release];
        [services release];
        [appMenu addItem:[NSMenuItem separatorItem]];

        [appMenu addItemWithTitle:[@"Hide " stringByAppendingString:name] action:@selector(hide:) keyEquivalent:@"h"];
        NSMenuItem* others = [appMenu addItemWithTitle:@"Hide Others" action:@selector(hideOtherApplications:) keyEquivalent:@"h"];
        others.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagOption;
        [appMenu addItemWithTitle:@"Show All" action:@selector(unhideAllApplications:) keyEquivalent:@""];
        [appMenu addItem:[NSMenuItem separatorItem]];

        NSMenuItem* quitItem = [appMenu addItemWithTitle:[@"Quit " stringByAppendingString:name] action:@selector(chosen:) keyEquivalent:@"q"];
        const auto tag = nextTag++;
        actions[tag] = quit;
        tagsOf[-1].push_back(tag);
        quitItem.tag = tag;
        quitItem.target = target;
    }

    void build()
    {
        // The menu bar is the app's, made here if the program hasn't made it yet.
        [NSApplication sharedApplication];
        bar = [[NSMenu alloc] initWithTitle:@"Main"];

        NSMenuItem* appEntry = [[NSMenuItem alloc] initWithTitle:programName() action:nil keyEquivalent:@""];
        appMenu = [[NSMenu alloc] initWithTitle:programName()];
        appEntry.submenu = appMenu;
        [bar addItem:appEntry];
        [appEntry release];
        fillMenu(-1);

        for (std::size_t i = 0; i < menus.size(); ++i)
        {
            NSMenuItem* entry = [[NSMenuItem alloc] initWithTitle:text(menus[i].title) action:nil keyEquivalent:@""];
            NSMenu* menu = [[NSMenu alloc] initWithTitle:text(menus[i].title)];
            menu.delegate = target;
            entry.submenu = menu;
            [bar addItem:entry];
            shown.push_back(menu);
            [entry release];
            fillMenu(static_cast<int>(i));
        }

        [NSApp setMainMenu:bar];
    }
};

MenuBar::MenuBar(std::vector<MenuItem> appItems, std::vector<Menu> menus, std::function<void()> quit)
    : impl(std::make_unique<Impl>())
{
    auto& m = *impl;
    m.appItems = std::move(appItems);
    m.menus = std::move(menus);
    m.quit = std::move(quit);
    m.target = [[Impl::targetClass() alloc] init];
    objc_setAssociatedObject(m.target, &implKey, [NSValue valueWithPointer:impl.get()], OBJC_ASSOCIATION_RETAIN);
    m.build();

    // While a menu is open its items stay as they are; what changed meanwhile is shown once the menus close.
    auto* notifications = [NSNotificationCenter defaultCenter];
    Impl* state = impl.get();
    m.observers.push_back([[notifications addObserverForName:NSMenuDidBeginTrackingNotification
                                                      object:m.bar
                                                       queue:nil
                                                  usingBlock:^(NSNotification*) {
                                                    state->tracking = true;
                                                  }] retain]);
    m.observers.push_back([[notifications addObserverForName:NSMenuDidEndTrackingNotification
                                                      object:m.bar
                                                       queue:nil
                                                  usingBlock:^(NSNotification*) {
                                                    state->tracking = false;
                                                    if (std::exchange(state->changedWhileTracking, false))
                                                        for (std::size_t i = 0; i < state->shown.size(); ++i)
                                                            state->fillMenu(static_cast<int>(i));
                                                  }] retain]);
}

MenuBar::~MenuBar()
{
    auto& m = *impl;
    for (id observer : m.observers)
    {
        [[NSNotificationCenter defaultCenter] removeObserver:observer];
        [observer release];
    }

    if ([NSApp mainMenu] == m.bar)
        [NSApp setMainMenu:nil];

    for (NSMenu* menu : m.shown)
    {
        menu.delegate = nil;
        [menu release];
    }
    [m.appMenu release];
    [m.bar release];
    objc_setAssociatedObject(m.target, &implKey, nil, OBJC_ASSOCIATION_RETAIN);
    [m.target release];
}

void MenuBar::changed()
{
    auto& m = *impl;
    if (m.tracking)
    {
        m.changedWhileTracking = true;
        return;
    }

    for (std::size_t i = 0; i < m.shown.size(); ++i)
        m.fillMenu(static_cast<int>(i));
}

}
