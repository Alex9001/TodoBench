// SPDX-License-Identifier: GPL-3.0-or-later
// NSWorkspace uses LaunchServices; do not replace this with exec of Contents/MacOS.
// https://developer.apple.com/documentation/appkit/nsworkspace/openconfiguration
#import <Cocoa/Cocoa.h>
int main(int argc, const char* argv[]) {
    @autoreleasepool {
        if (argc < 3) return 2;
        [NSApplication sharedApplication];
        NSWorkspaceOpenConfiguration* config = [NSWorkspaceOpenConfiguration configuration];
        config.activates = YES;
        config.createsNewApplicationInstance = YES;
        config.addsToRecentItems = NO;
        config.allowsRunningApplicationSubstitution = NO;
        NSMutableArray<NSString*>* arguments = [NSMutableArray array];
        for (int i = 3; i < argc; ++i) [arguments addObject:[NSString stringWithUTF8String:argv[i]]];
        config.arguments = arguments;
        config.environment = @{ @"QT_QPA_PLATFORM": @"cocoa" };
        __block BOOL finished = NO;
        __block int status = 1;
        NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[1]]];
        NSString* pidPath = [NSString stringWithUTF8String:argv[2]];
        [[NSWorkspace sharedWorkspace] openApplicationAtURL:url configuration:config completionHandler:^(NSRunningApplication* app, NSError* error) {
            if (error) fprintf(stderr, "%s\n", error.description.UTF8String);
            else {
                NSString* pid = [NSString stringWithFormat:@"%d", app.processIdentifier];
                status = [pid writeToFile:pidPath atomically:YES encoding:NSUTF8StringEncoding error:nil] ? 0 : 1;
            }
            finished = YES;
        }];
        NSDate* deadline = [NSDate dateWithTimeIntervalSinceNow:15];
        while (!finished && [deadline timeIntervalSinceNow] > 0)
            [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
        return status;
    }
}
