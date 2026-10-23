#include <functional>
#include <memory>
#include <string>

#import <AppKit/AppKit.h>
#import <WebKit/WebKit.h>

namespace symbolengine::ui::detail
{

void snapshot(void* view, const std::string& png, std::function<void(bool)> done)
{
    WKWebView* webView = (__bridge WKWebView*) view;
    WKSnapshotConfiguration* configuration = [[WKSnapshotConfiguration alloc] init];
    NSString* file = [NSString stringWithUTF8String:png.c_str()];
    auto finished = std::make_shared<std::function<void(bool)>>(std::move(done));

    [webView takeSnapshotWithConfiguration:configuration
                         completionHandler:^(NSImage* image, NSError*) {
                           bool written = false;
                           CGImageRef picture = image != nil ? [image CGImageForProposedRect:nil context:nil hints:nil]
                                                             : nullptr;
                           if (picture != nullptr && file != nil)
                           {
                               NSBitmapImageRep* bitmap = [[NSBitmapImageRep alloc] initWithCGImage:picture];
                               NSData* data = [bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
                               written = [data writeToFile:file atomically:YES];
                               [bitmap release];
                           }
                           (*finished)(written);
                         }];

    [configuration release];
}

}
