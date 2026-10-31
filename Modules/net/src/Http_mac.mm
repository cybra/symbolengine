#include "symbolengine/net/Http.h"

#import <Foundation/Foundation.h>
#include <objc/runtime.h>

#include <cstdint>
#include <exception>
#include <functional>
#include <string>
#include <string_view>

namespace
{
using Received = std::function<bool(int, std::string_view)>;

struct Fetch
{
    const Received* received = nullptr;
    symbolengine::http::Answer answer;
    bool stopped = false;
    std::exception_ptr thrown;
    dispatch_semaphore_t finished = nullptr;
};

Fetch& fetchOf(id delegate)
{
    return **static_cast<Fetch**>(object_getIndexedIvars(delegate));
}

// The session's delegate. Its class is made at run time under a name of its own, as plugins built on different versions of
// this code can share a process, where a class named when it was built would be one for them all.
Class delegateClass()
{
    static Class made = [] {
        const auto name = "SymbolEngineHttpFetch_" + std::to_string(reinterpret_cast<std::uintptr_t>(&fetchOf));
        Class c = objc_allocateClassPair([NSObject class], name.c_str(), 0);
        class_addProtocol(c, @protocol(NSURLSessionDataDelegate));

        class_addMethod(c, @selector(URLSession:dataTask:didReceiveResponse:completionHandler:),
                        (IMP) + [](id self, SEL, NSURLSession*, NSURLSessionDataTask*, NSURLResponse* response,
                                   void (^handler)(NSURLSessionResponseDisposition)) {
                            if ([response isKindOfClass:[NSHTTPURLResponse class]])
                                fetchOf(self).answer.status = static_cast<int>([static_cast<NSHTTPURLResponse*>(response) statusCode]);
                            handler(NSURLSessionResponseAllow);
                        },
                        "v@:@@@@?");

        class_addMethod(c, @selector(URLSession:dataTask:didReceiveData:),
                        (IMP) + [](id self, SEL, NSURLSession*, NSURLSessionDataTask* task, NSData* data) {
                            Fetch& fetch = fetchOf(self);
                            [data enumerateByteRangesUsingBlock:^(const void* bytes, NSRange range, BOOL* stop) {
                              if (fetch.stopped)
                                  return;

                              // Nothing can catch an exception on this thread, so it waits to be thrown again on the one asking.
                              bool going = false;
                              try
                              {
                                  going = (*fetch.received)(fetch.answer.status,
                                                            std::string_view(static_cast<const char*>(bytes), range.length));
                              }
                              catch (...)
                              {
                                  fetch.thrown = std::current_exception();
                              }

                              if (!going)
                              {
                                  fetch.stopped = true;
                                  *stop = YES;
                                  [task cancel];
                              }
                            }];
                        },
                        "v@:@@@");

        class_addMethod(c, @selector(URLSession:task:didCompleteWithError:),
                        (IMP) + [](id self, SEL, NSURLSession*, NSURLSessionTask*, NSError* error) {
                            Fetch& fetch = fetchOf(self);
                            if (error != nil && !fetch.stopped)
                                fetch.answer.problem = [[error localizedDescription] UTF8String];
                            dispatch_semaphore_signal(fetch.finished);
                        },
                        "v@:@@@");

        objc_registerClassPair(c);
        return c;
    }();
    return made;
}
}

namespace symbolengine::http
{

bool available()
{
    return true;
}

Answer get(const Request& request, const std::function<bool(int status, std::string_view piece)>& received)
{
    @autoreleasepool
    {
        NSURL* address = [NSURL URLWithString:[NSString stringWithUTF8String:request.url.c_str()]];
        NSString* scheme = address != nil ? [[address scheme] lowercaseString] : nil;

        if (scheme == nil || !([scheme isEqualToString:@"http"] || [scheme isEqualToString:@"https"]))
            return {0, "That isn't a web address: " + request.url};

        const auto seconds = std::chrono::duration<double>(request.timeout).count();
        NSMutableURLRequest* asked = [NSMutableURLRequest requestWithURL:address
                                                             cachePolicy:NSURLRequestReloadIgnoringLocalCacheData
                                                         timeoutInterval:seconds];

        for (const auto& [name, value] : request.headers)
            [asked setValue:[NSString stringWithUTF8String:value.c_str()]
                forHTTPHeaderField:[NSString stringWithUTF8String:name.c_str()]];

        NSURLSessionConfiguration* configuration = [NSURLSessionConfiguration ephemeralSessionConfiguration];

        Fetch fetch;
        fetch.received = &received;
        fetch.finished = dispatch_semaphore_create(0);

        // The session holds this until it has been let go of, later than this call, and calls nothing on it after the
        // task has completed.
        id delegate = class_createInstance(delegateClass(), sizeof(Fetch*));
        *static_cast<Fetch**>(object_getIndexedIvars(delegate)) = &fetch;

        NSOperationQueue* queue = [[NSOperationQueue alloc] init];
        queue.maxConcurrentOperationCount = 1;
        NSURLSession* session =
            [NSURLSession sessionWithConfiguration:configuration delegate:delegate delegateQueue:queue];
        [[session dataTaskWithRequest:asked] resume];
        dispatch_semaphore_wait(fetch.finished, DISPATCH_TIME_FOREVER);
        [session finishTasksAndInvalidate];

        dispatch_release(fetch.finished);
        [queue release];
        [delegate release];

        if (fetch.thrown != nullptr)
            std::rethrow_exception(fetch.thrown);

        return fetch.answer;
    }
}

}
