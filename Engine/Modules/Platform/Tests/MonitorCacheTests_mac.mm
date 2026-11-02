#include "MonitorCacheTests.h"

#import <AppKit/AppKit.h>

using GameEngine::Platform::EnumerateMonitors;
using GameEngine::Tests::AnyMonitorForcedHdr;
using GameEngine::Tests::MonitorCacheTests;

// AppKit posts this notification on the main thread when a display changes its
// resolution, arrangement or refresh rate, or connects or disconnects.
TEST_F(MonitorCacheTests, DisplayChangeNotificationQueriesThemAgain)
{
    [[NSNotificationCenter defaultCenter] postNotificationName:NSApplicationDidChangeScreenParametersNotification
                                                        object:NSApp];
    EXPECT_TRUE(AnyMonitorForcedHdr(EnumerateMonitors()));
}
