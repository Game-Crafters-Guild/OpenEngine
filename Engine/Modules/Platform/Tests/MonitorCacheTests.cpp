#include "MonitorCacheTests.h"

using GameEngine::Platform::EnumerateMonitors;
using GameEngine::Platform::InvalidateMonitors;
using GameEngine::Platform::MonitorInfo;
using GameEngine::Platform::Window;
using GameEngine::Tests::AnyMonitorForcedHdr;
using GameEngine::Tests::MonitorCacheTests;

TEST_F(MonitorCacheTests, AFrameDoesNotQueryTheMonitorsAgain)
{
    Window::PollEvents();
    const std::vector<MonitorInfo> afterFrame = EnumerateMonitors();
    ASSERT_EQ(afterFrame.size(), m_Initial.size());
    EXPECT_FALSE(AnyMonitorForcedHdr(afterFrame)) << "a frame re-read the monitors without a display change";
}

TEST_F(MonitorCacheTests, InvalidateMonitorsQueriesThemAgain)
{
    InvalidateMonitors();
    EXPECT_TRUE(AnyMonitorForcedHdr(EnumerateMonitors()));
}
