#include "WindowFrame_Win32.h"

#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <utility>

namespace GameEngine::Platform
{
namespace
{
// Room for the app mark, overflow heading and a drag target, in addition to
// the native caption buttons. Keep snapped windows usable at their minimum size.
constexpr int kMinimumCaptionContentWidth = 160;

int ResizeBorder(HWND window)
{
    const UINT dpi = GetDpiForWindow(window);
    return GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
}

template <typename Region>
bool Contains(const Region& region, POINT point)
{
    return point.x >= region.X && point.x < region.X + region.Width &&
           point.y >= region.Y && point.y < region.Y + region.Height;
}
} // namespace

TitleBarMetrics WindowFrame::GetMetrics(HWND window) const
{
    const UINT dpi = GetDpiForWindow(window);
    RECT client{};
    GetClientRect(window, &client);
    // The system's caption-button bounds are window-relative, not client-relative.
    RECT buttons{};
    int captionWidth = 3 * GetSystemMetricsForDpi(SM_CXSIZE, dpi);
    if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CAPTION_BUTTON_BOUNDS, &buttons, sizeof(buttons))) &&
        buttons.right > buttons.left)
    {
        RECT outer{};
        GetWindowRect(window, &outer);
        POINT clientRight{};
        clientRight.x = client.right;
        ClientToScreen(window, &clientRight);
        captionWidth = static_cast<int>(clientRight.x - outer.left - buttons.left);
    }
    const int height = GetSystemMetricsForDpi(SM_CYCAPTION, dpi) + ResizeBorder(window);
    TitleBarMetrics metrics;
    metrics.Height = height;
    metrics.CaptionWidth = std::clamp(captionWidth, 0, static_cast<int>(client.right));
    metrics.HoveredButton = m_HoveredButton;
    metrics.Maximized = IsZoomed(window) != FALSE;
    constexpr TitleBarButton kButtons[] = {TitleBarButton::Minimize, TitleBarButton::Maximize, TitleBarButton::Close};
    for (size_t index = 0; index < metrics.Buttons.size(); ++index)
    {
        const int left = static_cast<int>(client.right) - metrics.CaptionWidth +
                         metrics.CaptionWidth * static_cast<int>(index) / 3;
        const int right = static_cast<int>(client.right) - metrics.CaptionWidth +
                          metrics.CaptionWidth * static_cast<int>(index + 1) / 3;
        metrics.Buttons[index] = {kButtons[index], left, 0, right - left, m_Height > 0 ? m_Height : height};
    }
    return metrics;
}

void WindowFrame::SetLayout(HWND window, int height, std::vector<TitleBarMenuRegion> regions)
{
    const bool frameChanged = (m_Height == 0) != (height == 0);
    const bool heightChanged = m_Height != height;
    m_Height = std::max(0, height);
    if (m_Height == 0)
        m_HoveredButton = TitleBarButton::None;
    m_Regions = std::move(regions);
    if (!heightChanged)
        return;

    const MARGINS margins{0, 0, m_Height, 0};
    DwmExtendFrameIntoClientArea(window, &margins);
    if (frameChanged)
        SetWindowPos(window, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

bool WindowFrame::HandleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result)
{
    if (m_Height == 0)
        return false;

    if (message == WM_NCMOUSEMOVE)
    {
        m_HoveredButton = wParam == HTMINBUTTON ? TitleBarButton::Minimize :
                          wParam == HTMAXBUTTON ? TitleBarButton::Maximize :
                          wParam == HTCLOSE ? TitleBarButton::Close : TitleBarButton::None;
        TRACKMOUSEEVENT tracking{sizeof(TRACKMOUSEEVENT), TME_LEAVE | TME_NONCLIENT, window, 0};
        TrackMouseEvent(&tracking);
    }
    if (message == WM_NCMOUSELEAVE || message == WM_MOUSEMOVE || (message == WM_ACTIVATE && LOWORD(wParam) == WA_INACTIVE))
        m_HoveredButton = TitleBarButton::None;

    if (message == WM_GETMINMAXINFO)
    {
        auto& info = *reinterpret_cast<MINMAXINFO*>(lParam);
        const int contentWidth = MulDiv(kMinimumCaptionContentWidth, static_cast<int>(GetDpiForWindow(window)), 96);
        info.ptMinTrackSize.x = std::max(info.ptMinTrackSize.x,
                                        static_cast<LONG>(GetMetrics(window).CaptionWidth + contentWidth));
        result = 0;
        return true;
    }

    if (message == WM_NCCALCSIZE && wParam)
    {
        auto& rect = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam)->rgrc[0];
        // Keep the ordinary side/bottom resize frame. A maximized window's outer
        // rectangle includes an invisible top border outside the monitor work area.
        const int border = ResizeBorder(window);
        rect.left += border;
        rect.right -= border;
        rect.bottom -= border;
        if (IsZoomed(window))
            rect.top += border;
        result = 0;
        return true;
    }

    if (message == WM_NCHITTEST)
    {
        if (DwmDefWindowProc(window, message, wParam, lParam, &result))
            return true;

        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(window, &point);
        RECT client{};
        GetClientRect(window, &client);
        // The renderer covers the DWM caption surface. Match its painted buttons
        // even where DWM does not supply a hit (for example the taller CSS row).
        const auto metrics = GetMetrics(window);
        constexpr LRESULT kHits[] = {HTMINBUTTON, HTMAXBUTTON, HTCLOSE};
        for (size_t index = 0; index < metrics.Buttons.size(); ++index)
        {
            if (Contains(metrics.Buttons[index], point))
            {
                result = kHits[index];
                return true;
            }
        }
        const int border = ResizeBorder(window);
        if (!IsZoomed(window) && point.y < border)
        {
            result = point.x < border ? HTTOPLEFT : (point.x >= client.right - border ? HTTOPRIGHT : HTTOP);
            return true;
        }
        // Let GLFW/DefWindowProc handle the native side and bottom frame.
        if (point.x < 0 || point.x >= client.right || point.y < 0 || point.y >= client.bottom)
            return false;
        if (point.y < m_Height)
        {
            for (const auto& region : m_Regions)
            {
                if (Contains(region, point))
                {
                    result = HTCLIENT;
                    return true;
                }
            }
            result = HTCAPTION;
            return true;
        }
    }

    if (message == WM_DWMCOMPOSITIONCHANGED || message == WM_ACTIVATE)
    {
        const MARGINS margins{0, 0, m_Height, 0};
        DwmExtendFrameIntoClientArea(window, &margins);
    }
    // DWM also needs non-client hover/leave messages for native caption buttons.
    if (DwmDefWindowProc(window, message, wParam, lParam, &result))
        return true;
    return false;
}

} // namespace GameEngine::Platform
