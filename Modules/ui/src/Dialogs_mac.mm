#include <symbolengine/ui/Dialogs.h>

#include <symbolengine/core/Paths.h>

#import <AppKit/AppKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

namespace symbolengine::ui
{

namespace
{
NSString* text(const std::string& value)
{
    NSString* converted = [NSString stringWithUTF8String:value.c_str()];
    return converted != nil ? converted : @"";
}

NSWindow* windowOf(void* parent)
{
    if (parent == nullptr)
        return nil;

    id object = (__bridge id) parent;
    if ([object isKindOfClass:[NSWindow class]])
        return (NSWindow*) object;
    if ([object isKindOfClass:[NSView class]])
        return [(NSView*) object window];
    return nil;
}

NSArray<UTType*>* typesFor(const std::vector<std::string>& extensions)
{
    NSMutableArray<UTType*>* types = [NSMutableArray array];
    for (auto extension : extensions)
    {
        if (!extension.empty() && extension.front() == '.')
            extension.erase(0, 1);
        if (UTType* type = [UTType typeWithFilenameExtension:text(extension)])
            [types addObject:type];
    }
    return types;
}
}

void ask(const Question& question, void* parent, std::function<void(std::size_t chosen)> answered)
{
    NSWindow* window = [windowOf(parent) retain];
    auto asked = question;

    dispatch_async(dispatch_get_main_queue(), ^{
      @autoreleasepool
      {
          NSAlert* alert = [[NSAlert alloc] init];
          alert.messageText = text(asked.title);
          alert.informativeText = text(asked.message);
          alert.alertStyle = asked.kind == Question::Kind::warning ? NSAlertStyleWarning : NSAlertStyleInformational;

          // The first button takes Return; a cancel button other than the first takes Escape.
          for (std::size_t i = 0; i < asked.buttons.size(); ++i)
          {
              NSButton* button = [alert addButtonWithTitle:text(asked.buttons[i])];
              if (asked.cancel == i && i > 0)
                  button.keyEquivalent = @"\033";
          }

          const auto count = asked.buttons.size();
          const auto cancel = asked.cancel.value_or(0);
          const auto answer = [answered, count, cancel](NSModalResponse response) {
              const auto index = static_cast<std::size_t>(response - NSAlertFirstButtonReturn);
              answered(index < count ? index : cancel);
          };

          if (window != nil && window.visible)
          {
              [alert beginSheetModalForWindow:window
                            completionHandler:^(NSModalResponse response) {
                              answer(response);
                              [alert release];
                            }];
          }
          else
          {
              const auto response = [alert runModal];
              [alert release];
              answer(response);
          }

          [window release];
      }
    });
}

void chooseFile(const FileChoice& choice, void*, std::function<void(std::optional<std::filesystem::path> chosen)> chosen)
{
    auto asked = choice;

    dispatch_async(dispatch_get_main_queue(), ^{
      @autoreleasepool
      {
          NSSavePanel* panel = nil;
          std::filesystem::path folder = asked.start;

          if (asked.mode == FileChoice::Mode::save)
          {
              panel = [NSSavePanel savePanel];
              panel.canCreateDirectories = YES;
              if (asked.start.has_filename())
              {
                  panel.nameFieldStringValue = text(paths::toUtf8(asked.start.filename()));
                  folder = asked.start.parent_path();
              }
          }
          else
          {
              NSOpenPanel* open = [NSOpenPanel openPanel];
              open.canChooseFiles = asked.mode == FileChoice::Mode::openFile;
              open.canChooseDirectories = asked.mode == FileChoice::Mode::openFolder;
              open.allowsMultipleSelection = NO;
              panel = open;
          }

          panel.title = text(asked.title);
          if (!folder.empty())
              panel.directoryURL = [NSURL fileURLWithPath:text(paths::toUtf8(folder)) isDirectory:YES];
          if (!asked.extensions.empty() && asked.mode != FileChoice::Mode::openFolder)
              panel.allowedContentTypes = typesFor(asked.extensions);

          std::optional<std::filesystem::path> result;
          if ([panel runModal] == NSModalResponseOK && panel.URL != nil && panel.URL.fileURL)
              result = paths::fromUtf8(panel.URL.fileSystemRepresentation);

          chosen(result);
      }
    });
}

}
