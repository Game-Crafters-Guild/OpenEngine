#include "MonitorCacheTests.h"
#include "Display/DisplayChangeListener.h"

#include <cwchar>
#include <iterator>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

using GameEngine::Platform::EnumerateMonitors;
using GameEngine::Platform::Window;
using GameEngine::Tests::AnyMonitorForcedHdr;
using GameEngine::Tests::MonitorCacheTests;

namespace
{

BOOL CALLBACK FindListenerWindow(HWND window, LPARAM result)
{
    wchar_t className[64] = {};
    GetClassNameW(window, className, static_cast<int>(std::size(className)));
    if (std::wcscmp(className, GameEngine::Platform::kDisplayChangeListenerClassName) != 0)
        return TRUE;
    *reinterpret_cast<HWND*>(result) = window;
    return FALSE;
}

} // namespace

TEST_F(MonitorCacheTests, DisplayChangeMessageQueriesThemAgain)
{
    HWND listener = nullptr;
    EnumThreadWindows(GetCurrentThreadId(), &FindListenerWindow, reinterpret_cast<LPARAM>(&listener));
    ASSERT_NE(listener, nullptr) << "Window::Initialize did not create the display-change listener";

    SendMessageW(listener, WM_DISPLAYCHANGE, 32, MAKELPARAM(1920, 1080));
    Window::PollEvents();
    EXPECT_TRUE(AnyMonitorForcedHdr(EnumerateMonitors()));
}
