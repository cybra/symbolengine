#include "Platform.h"

#include <symbolengine/core/Paths.h>
#include <symbolengine/ui/MessageLoop.h>

#import <AppKit/AppKit.h>
#include <objc/runtime.h>

#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace symbolengine::app::platform
{

namespace
{
id delegate = nil;
int instanceLock = -1;       // the file held locked by the running instance
NSString* instanceNotice = nil;   // the notification a second instance posts to it

NSString* nsString(const std::string& text)
{
    return [NSString stringWithUTF8String:text.c_str()];
}

std::string stdString(NSString* text)
{
    return text.UTF8String != nullptr ? text.UTF8String : "";
}

// The delegate's class: it passes each request of the system's to the Application, and leaves the system's own
// handling in place where that is right.
Class delegateClass()
{
    static Class made = [] {
        Class c = objc_allocateClassPair([NSObject class], "SymbolEngineApplicationDelegate", 0);

        // Quit from the Dock, logging out, shutting down: the program is asked, and quits by stopping its loop.
        class_addMethod(c, @selector(applicationShouldTerminate:),
                        (IMP) + [](id, SEL, id) -> NSUInteger {
                            if (auto* app = Application::current())
                                app->requestQuit();
                            return NSTerminateCancel;
                        },
                        "L@:@");

        class_addMethod(c, @selector(application:openFiles:),
                        (IMP) + [](id, SEL, id sender, NSArray<NSString*>* names) {
                            std::vector<std::filesystem::path> files;
                            for (NSString* name in names)
                                files.push_back(paths::fromUtf8(stdString(name)));
                            if (auto* app = Application::current(); app != nullptr && app->onOpenFiles)
                                app->onOpenFiles(std::move(files));
                            [static_cast<NSApplication*>(sender) replyToOpenOrPrint:NSApplicationDelegateReplySuccess];
                        },
                        "v@:@@");

        class_addMethod(c, @selector(applicationShouldHandleReopen:hasVisibleWindows:),
                        (IMP) + [](id, SEL, id, BOOL hasVisibleWindows) -> BOOL {
                            if (auto* app = Application::current(); app != nullptr && app->onReopen && !hasVisibleWindows)
                                app->onReopen();
                            return YES;
                        },
                        "c@:@c");

        class_addMethod(c, @selector(anotherInstanceStarted:),
                        (IMP) + [](id, SEL, NSNotification* notification) {
                            std::vector<std::string> arguments;
                            if (NSArray* given = notification.userInfo[@"arguments"]; [given isKindOfClass:[NSArray class]])
                                for (NSString* argument in given)
                                    if ([argument isKindOfClass:[NSString class]])
                                        arguments.push_back(stdString(argument));
                            if (auto* app = Application::current(); app != nullptr && app->onAnotherInstance)
                                app->onAnotherInstance(std::move(arguments));
                        },
                        "v@:@");

        objc_registerClassPair(c);
        return c;
    }();
    return made;
}
}

void start(Application&, const std::string& identifier)
{
    [NSApplication sharedApplication];
    delegate = [[delegateClass() alloc] init];
    [NSApp setDelegate:delegate];
    instanceNotice = [nsString(identifier + ".symbolengine.instance") retain];
}

void finish()
{
    [[NSDistributedNotificationCenter defaultCenter] removeObserver:delegate];
    if ([NSApp delegate] == delegate)
        [NSApp setDelegate:nil];
    [delegate release];
    delegate = nil;
    [instanceNotice release];
    instanceNotice = nil;
    if (instanceLock >= 0)
    {
        close(instanceLock);   // which lets go of the lock
        instanceLock = -1;
    }
}

std::vector<std::string> arguments(int argc, char** argv)
{
    std::vector<std::string> given;
    for (int i = 1; i < argc && argv != nullptr; ++i)
        if (argv[i] != nullptr)
            given.emplace_back(argv[i]);
    return given;
}

// The running instance holds a lock on a file named after the program in the user's temporary folder, which the
// system lets go of however the process ends, and hears from later instances through the user's distributed
// notifications.
bool sendToRunningInstance(const std::string& identifier, const std::vector<std::string>& arguments)
{
    if (instanceLock >= 0)
        return false;

    std::error_code ignored;
    const auto folder = paths::systemFolder(paths::Folder::temp, ignored);
    if (folder.empty())
        return false;

    const auto file = folder / (identifier + ".instance");
    const int fd = open(file.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0)
        return false;

    if (flock(fd, LOCK_EX | LOCK_NB) == 0)
    {
        instanceLock = fd;
        [[NSDistributedNotificationCenter defaultCenter]
                   addObserver:delegate
                      selector:@selector(anotherInstanceStarted:)
                          name:instanceNotice
                        object:nil
            suspensionBehavior:NSNotificationSuspensionBehaviorDeliverImmediately];
        return false;
    }

    const bool held = errno == EWOULDBLOCK;
    close(fd);
    if (!held)
        return false;

    NSMutableArray<NSString*>* given = [NSMutableArray arrayWithCapacity:arguments.size()];
    for (const auto& argument : arguments)
        [given addObject:nsString(argument)];
    [[NSDistributedNotificationCenter defaultCenter] postNotificationName:instanceNotice
                                                                   object:nil
                                                                 userInfo:@{@"arguments" : given}
                                                       deliverImmediately:YES];
    return true;
}

bool isForeground()
{
    return [NSApp isActive];
}

void setDockIconVisible(bool visible)
{
    [NSApp setActivationPolicy:visible ? NSApplicationActivationPolicyRegular : NSApplicationActivationPolicyAccessory];
}

void noteRecentFile(const std::filesystem::path& file)
{
    if (NSURL* url = [NSURL fileURLWithPath:nsString(paths::toUtf8(file))])
        [[NSDocumentController sharedDocumentController] noteNewRecentDocumentURL:url];
}

void terminate(int code)
{
    _exit(code);
}

}
