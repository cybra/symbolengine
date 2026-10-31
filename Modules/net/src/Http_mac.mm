#include "symbolengine/net/Http.h"

#import <Foundation/Foundation.h>

#include <exception>
#include <string>

using Received = std::function<bool(int, std::string_view)>;

@interface SymbolEngineHttpFetch : NSObject <NSURLSessionDataDelegate> {
@public
    const Received* received;
    symbolengine::http::Answer answer;
    bool stopped;
    std::exception_ptr thrown;
    dispatch_semaphore_t finished;
}
@end

@implementation SymbolEngineHttpFetch

- (void)URLSession:(NSURLSession*)session
              dataTask:(NSURLSessionDataTask*)task
    didReceiveResponse:(NSURLResponse*)response
     completionHandler:(void (^)(NSURLSessionResponseDisposition))handler
{
    if ([response isKindOfClass:[NSHTTPURLResponse class]])
        answer.status = static_cast<int>([static_cast<NSHTTPURLResponse*>(response) statusCode]);

    handler(NSURLSessionResponseAllow);
}

- (void)URLSession:(NSURLSession*)session dataTask:(NSURLSessionDataTask*)task didReceiveData:(NSData*)data
{
    [data enumerateByteRangesUsingBlock:^(const void* bytes, NSRange range, BOOL* stop) {
      if (stopped)
          return;

      // Nothing can catch an exception on this thread, so it waits to be thrown again on the one asking.
      bool going = false;
      try
      {
          going = (*received)(answer.status, std::string_view(static_cast<const char*>(bytes), range.length));
      }
      catch (...)
      {
          thrown = std::current_exception();
      }

      if (!going)
      {
          stopped = true;
          *stop = YES;
          [task cancel];
      }
    }];
}

- (void)URLSession:(NSURLSession*)session task:(NSURLSessionTask*)task didCompleteWithError:(NSError*)error
{
    if (error != nil && !stopped)
        answer.problem = [[error localizedDescription] UTF8String];

    dispatch_semaphore_signal(finished);
}

@end

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

        SymbolEngineHttpFetch* fetch = [[SymbolEngineHttpFetch alloc] init];
        fetch->received = &received;
        fetch->stopped = false;
        fetch->finished = dispatch_semaphore_create(0);

        NSOperationQueue* queue = [[NSOperationQueue alloc] init];
        queue.maxConcurrentOperationCount = 1;
        NSURLSession* session =
            [NSURLSession sessionWithConfiguration:configuration delegate:fetch delegateQueue:queue];
        [[session dataTaskWithRequest:asked] resume];
        dispatch_semaphore_wait(fetch->finished, DISPATCH_TIME_FOREVER);
        [session finishTasksAndInvalidate];

        Answer answer = fetch->answer;
        const auto thrown = fetch->thrown;
        dispatch_release(fetch->finished);
        [queue release];
        [fetch release];

        if (thrown != nullptr)
            std::rethrow_exception(thrown);

        return answer;
    }
}

}
