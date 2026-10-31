#include "Platform.h"

#include <symbolengine/core/Paths.h>
#include <symbolengine/ui/MessageLoop.h>

#import <AppKit/AppKit.h>
#include <objc/runtime.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace symbolengine::app::platform
{

namespace
{
id delegate = nil;
int instanceLock = -1;       // the file held locked by the running instance
dispatch_source_t instanceListening = nullptr;   // the socket later instances hand over through
std::filesystem::path instanceSocket;

// The system's requests to quit that are waiting for their answer, which is that the program has gone, or that it won't.
std::vector<NSAppleEventManagerSuspensionID> quitsWaiting;

NSString* nsString(const std::string& text)
{
    return [NSString stringWithUTF8String:text.c_str()];
}

std::string stdString(NSString* text)
{
    return text.UTF8String != nullptr ? text.UTF8String : "";
}

// Later instances reach the running one through a socket in the user's own temporary folder, which no other user or
// sandboxed program can reach, and the system says who connected. Its name is a number made from the identifier, as a
// socket's path has little room.
std::filesystem::path socketFor(const std::string& identifier, const std::filesystem::path& folder)
{
    std::uint64_t hash = 14695981039346656037ull;
    for (const auto c : identifier)
    {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ull;
    }
    return folder / ("symbolengine-" + std::to_string(hash) + ".instance");
}

bool addressOf(const std::filesystem::path& file, sockaddr_un& address)
{
    const auto name = file.native();
    if (name.size() >= sizeof address.sun_path)
        return false;
    address = {};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, name.c_str(), name.size() + 1);
    return true;
}

void setTimeLimits(int socketHandle)
{
    const timeval limit{5, 0};
    const int on = 1;
    setsockopt(socketHandle, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof limit);
    setsockopt(socketHandle, SOL_SOCKET, SO_SNDTIMEO, &limit, sizeof limit);
    setsockopt(socketHandle, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
}

bool sendAll(int socketHandle, const void* bytes, std::size_t size)
{
    const auto* at = static_cast<const char*>(bytes);
    while (size > 0)
    {
        const auto sent = write(socketHandle, at, size);
        if (sent <= 0)
            return false;
        at += sent;
        size -= static_cast<std::size_t>(sent);
    }
    return true;
}

bool readAll(int socketHandle, void* bytes, std::size_t size)
{
    auto* at = static_cast<char*>(bytes);
    while (size > 0)
    {
        const auto got = read(socketHandle, at, size);
        if (got <= 0)
            return false;
        at += got;
        size -= static_cast<std::size_t>(got);
    }
    return true;
}

// One later instance's arguments, each ended by a zero, after their size. It hears a single byte when they have been
// taken, which is how it knows it can go.
void takeHandOver(int listener)
{
    const int from = accept(listener, nullptr, nullptr);
    if (from < 0)
        return;

    constexpr std::uint32_t mostHandedOver = 16 * 1024 * 1024;
    uid_t user = 0;
    gid_t group = 0;
    std::uint32_t size = 0;
    std::string text;
    setTimeLimits(from);
    const bool taken = getpeereid(from, &user, &group) == 0 && user == getuid() && readAll(from, &size, sizeof size) &&
                       size <= mostHandedOver && (text.resize(size), readAll(from, text.data(), size));
    if (taken)
    {
        const char received = 1;
        sendAll(from, &received, 1);

        std::vector<std::string> arguments;
        std::size_t start = 0;
        for (auto end = text.find('\0'); end != std::string::npos; end = text.find('\0', start))
        {
            arguments.push_back(text.substr(start, end - start));
            start = end + 1;
        }
        ui::messageLoop::callAsync([arguments = std::move(arguments)]() mutable {
            if (auto* app = Application::current(); app != nullptr && app->onAnotherInstance)
                app->onAnotherInstance(std::move(arguments));
        });
    }
    close(from);
}

void startListening(const std::filesystem::path& file)
{
    sockaddr_un address{};
    if (!addressOf(file, address))
        return;

    const int listener = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listener < 0)
        return;

    // One left by a program that ended without tidying up: this one has the lock, so it is nobody's.
    unlink(file.c_str());
    fcntl(listener, F_SETFD, FD_CLOEXEC);
    if (bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof address) != 0 || chmod(file.c_str(), 0600) != 0 ||
        listen(listener, 8) != 0)
    {
        close(listener);
        return;
    }

    instanceSocket = file;
    instanceListening = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, static_cast<std::uintptr_t>(listener), 0,
                                               dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0));
    dispatch_source_set_event_handler(instanceListening, ^{
      takeHandOver(listener);
    });
    dispatch_source_set_cancel_handler(instanceListening, ^{
      close(listener);
    });
    dispatch_resume(instanceListening);
}

void stopListening()
{
    if (instanceListening == nullptr)
        return;
    dispatch_source_cancel(instanceListening);
    dispatch_release(instanceListening);
    instanceListening = nullptr;
    std::error_code ignored;
    std::filesystem::remove(instanceSocket, ignored);
}

// Says whether the running instance has taken the arguments, so this one can go. That one has the lock just before it
// starts listening, so it may need a moment.
bool handOver(const std::filesystem::path& file, const std::vector<std::string>& arguments)
{
    sockaddr_un address{};
    if (!addressOf(file, address))
        return false;

    int to = -1;
    for (int tries = 0; to < 0 && tries < 60; ++tries)
    {
        to = socket(AF_UNIX, SOCK_STREAM, 0);
        if (to >= 0 && connect(to, reinterpret_cast<const sockaddr*>(&address), sizeof address) != 0)
        {
            close(to);
            to = -1;
        }
        if (to < 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (to < 0)
        return false;

    std::string text;
    for (const auto& argument : arguments)
        text.append(argument).push_back('\0');
    const auto size = static_cast<std::uint32_t>(text.size());

    setTimeLimits(to);
    char received = 0;
    const bool taken = sendAll(to, &size, sizeof size) && sendAll(to, text.data(), text.size()) && readAll(to, &received, 1);
    close(to);
    if (!taken || received != 1)
        return false;

    // Its window can come forward now, if this one has been in front.
    if (@available(macOS 14, *))
        if (NSString* program = NSBundle.mainBundle.bundleIdentifier; program != nil)
            [NSApp yieldActivationToApplicationWithBundleIdentifier:program];

    return true;
}

void handleQuitEvents(id handler)
{
    [[NSAppleEventManager sharedAppleEventManager] setEventHandler:handler
                                                       andSelector:@selector(quitEvent:withReply:)
                                                     forEventClass:kCoreEventClass
                                                        andEventID:kAEQuitApplication];
}

void answerQuits(bool quitting)
{
    auto* const events = [NSAppleEventManager sharedAppleEventManager];
    for (const auto waiting : std::exchange(quitsWaiting, {}))
    {
        if (!quitting)
            [[events replyAppleEventForSuspensionID:waiting] setParamDescriptor:[NSAppleEventDescriptor descriptorWithInt32:userCanceledErr]
                                                                     forKeyword:keyErrorNumber];
        [events resumeWithSuspensionID:waiting];
    }
}

// The delegate's class: it passes each request of the system's to the Application, and leaves the system's own
// handling in place where that is right.
Class delegateClass()
{
    static Class made = [] {
        Class c = objc_allocateClassPair([NSObject class], "SymbolEngineApplicationDelegate", 0);

        // Anything else asking NSApplication to quit: the program is asked, and quits by stopping its loop.
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

        // The system's event to quit, sent for Quit in the Dock's menu and for logging out and shutting down. Answering it
        // through NSApplication could only be a refusal, which stops a logout, or an exit from inside the event, which
        // leaves run() never returning. So the answer waits, and the process ends the usual way.
        class_addMethod(c, @selector(quitEvent:withReply:),
                        (IMP) + [](id, SEL, NSAppleEventDescriptor*, NSAppleEventDescriptor*) {
                            auto* app = Application::current();
                            if (app == nullptr)
                                return;
                            quitsWaiting.push_back([[NSAppleEventManager sharedAppleEventManager] suspendCurrentAppleEvent]);
                            app->requestQuit();
                        },
                        "v@:@@");

        // Where a program's own handlers are installed, as NSApplication installs its own as it starts.
        class_addMethod(c, @selector(applicationWillFinishLaunching:), (IMP) + [](id self, SEL, NSNotification*) { handleQuitEvents(self); },
                        "v@:@");

        objc_registerClassPair(c);
        return c;
    }();
    return made;
}
}

void declineQuit()
{
    answerQuits(false);
}

void start(Application&, const std::string&)
{
    [NSApplication sharedApplication];
    delegate = [[delegateClass() alloc] init];
    [NSApp setDelegate:delegate];
    handleQuitEvents(delegate);
}

void finish()
{
    answerQuits(true);
    [[NSAppleEventManager sharedAppleEventManager] removeEventHandlerForEventClass:kCoreEventClass andEventID:kAEQuitApplication];
    if ([NSApp delegate] == delegate)
        [NSApp setDelegate:nil];
    [delegate release];
    delegate = nil;
    stopListening();
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
// system lets go of however the process ends, and hears from later instances through a socket beside it.
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

    const auto socketFile = socketFor(identifier, folder);
    if (flock(fd, LOCK_EX | LOCK_NB) == 0)
    {
        instanceLock = fd;
        startListening(socketFile);
        return false;
    }

    const bool held = errno == EWOULDBLOCK;
    close(fd);
    return held && handOver(socketFile, arguments);
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
