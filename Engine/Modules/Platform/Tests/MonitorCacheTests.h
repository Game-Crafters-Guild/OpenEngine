#pragma once

#include "Platform/Display.h"
#include "Platform/Window.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <string>
#include <vector>

namespace GameEngine::Tests
{

// The forced HDR profile marks every monitor it touches, so a monitor list
// read with the variable set differs from one read without it.
inline constexpr const char* kForceHdrVariable = "GE_FORCE_HDR_DISPLAY";

// An empty value clears the variable for the monitor read.
inline void SetForceHdr(const char* value)
{
#if defined(_WIN32)
    _putenv_s(kForceHdrVariable, value);
#else
    setenv(kForceHdrVariable, value, 1);
#endif
}

inline bool AnyMonitorForcedHdr(const std::vector<Platform::MonitorInfo>& monitors)
{
    for (const Platform::MonitorInfo& monitor : monitors)
    {
        for (const std::string& hint : monitor.diagnosticHints)
        {
            if (hint.find(kForceHdrVariable) != std::string::npos)
                return true;
        }
    }
    return false;
}

// Keeps a monitor list read without the forced profile, then sets the profile:
// a later read that shows the profile read the OS again.
class MonitorCacheTests : public testing::Test
{
  protected:
    void SetUp() override
    {
        ASSERT_TRUE(Platform::Window::Initialize());
        SetForceHdr("");
        Platform::InvalidateMonitors();
        m_Initial = Platform::EnumerateMonitors();
        if (m_Initial.empty())
            GTEST_SKIP() << "No monitor is attached; the cache has nothing to hold.";
        ASSERT_FALSE(AnyMonitorForcedHdr(m_Initial));
        SetForceHdr("1");
    }

    void TearDown() override
    {
        SetForceHdr("");
        Platform::InvalidateMonitors();
    }

    // The display-change listener is destroyed on this thread, not at static destruction.
    static void TearDownTestSuite() { Platform::Window::Terminate(); }

    std::vector<Platform::MonitorInfo> m_Initial;
};

} // namespace GameEngine::Tests
