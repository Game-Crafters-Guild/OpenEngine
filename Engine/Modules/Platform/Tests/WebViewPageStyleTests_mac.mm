#include "WebViewPageStyle_mac.h"

#include <gtest/gtest.h>

#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>

#include <chrono>

namespace
{
TEST(WebViewPageStyle, OverridesPageBackgroundBeforeDOMContentLoaded)
{
    @autoreleasepool
    {
        WKUserContentController* controller = [[WKUserContentController alloc] init];
        WKUserScript* style = [[WKUserScript alloc]
            initWithSource:GameEngine::Platform::Detail::kPageStyleScript
            injectionTime:WKUserScriptInjectionTimeAtDocumentStart
            forMainFrameOnly:YES];
        [controller addUserScript:style];
        WKWebViewConfiguration* configuration = [[WKWebViewConfiguration alloc] init];
        configuration.userContentController = controller;
        configuration.websiteDataStore = [WKWebsiteDataStore nonPersistentDataStore];
        WKWebView* view = [[WKWebView alloc] initWithFrame:NSMakeRect(0, 0, 320, 200) configuration:configuration];
        // The inline script records the color during parsing, before the
        // DOMContentLoaded handler has an opportunity to hide a first-paint flash.
        [view loadHTMLString:@"<!doctype html><html><head><style>body { background: #171717; }</style></head>"
            "<body><script>window.initialBackground = getComputedStyle(document.body).backgroundColor;</script></body></html>"
            baseURL:nil];

        __block bool finished = false;
        __block NSString* background = nil;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (!finished && std::chrono::steady_clock::now() < deadline)
        {
            if (!view.loading)
            {
                [view evaluateJavaScript:@"window.initialBackground" completionHandler:^(id result, NSError* error)
                {
                    if (!error && [result isKindOfClass:[NSString class]])
                    {
                        background = result;
                        finished = true;
                    }
                }];
            }
            [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
        }
        [view stopLoading];
        ASSERT_TRUE(finished) << "The off-screen WKWebView did not finish its probe.";
        EXPECT_STREQ([background UTF8String], "rgb(39, 39, 39)");
    }
}
}
