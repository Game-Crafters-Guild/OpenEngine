#pragma once

#include "Platform/Toolbar.h"

#include <windows.h>
#include <vector>

namespace GameEngine::Platform
{

/// Extends a GLFW window's client area into its caption, retaining native button behavior.
/// The toolbar owns the window procedure and forwards messages here before GLFW.
class WindowFrame
{
  public:
    void SetLayout(HWND window, int height, std::vector<TitleBarMenuRegion> regions);
    TitleBarMetrics GetMetrics(HWND window) const;
    bool HandleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result);

  private:
    int m_Height = 0;
    TitleBarButton m_HoveredButton = TitleBarButton::None;
    std::vector<TitleBarMenuRegion> m_Regions;
};

} // namespace GameEngine::Platform
