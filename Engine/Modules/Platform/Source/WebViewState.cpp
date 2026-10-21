#include "WebViewState.h"

#include <utility>

namespace GameEngine {
namespace Platform {

void WebViewState::Attach(IWebViewTarget& target)
{
    m_Target = &target;
    ApplyBackgroundColor();
    ApplyBounds();
    ApplyVisible();

    const Page pending = std::exchange(m_PendingKind, Page::None);
    const std::string page = std::move(m_PendingPage);
    m_PendingPage.clear();
    if (pending == Page::URL)
        m_Target->NavigateToURL(page);
    else if (pending == Page::HTML)
        m_Target->NavigateToHTML(page);
}

void WebViewState::Detach()
{
    m_Target = nullptr;
    m_HasAppliedBounds = false;
    m_HasAppliedVisibility = false;
}

bool WebViewState::LoadURL(const std::string& url)
{
    if (m_Target)
        return m_Target->NavigateToURL(url);

    m_PendingPage = url;
    m_PendingKind = Page::URL;
    return true;
}

bool WebViewState::LoadHTML(const std::string& html)
{
    if (m_Target)
        return m_Target->NavigateToHTML(html);

    m_PendingPage = html;
    m_PendingKind = Page::HTML;
    return true;
}

void WebViewState::Resize(int width, int height)
{
    m_Bounds.Right = m_Bounds.Left + width;
    m_Bounds.Bottom = m_Bounds.Top + height;
    m_HasBounds = true;
    ApplyBounds();
}

void WebViewState::SetPosition(int x, int y)
{
    const int width = m_Bounds.Right - m_Bounds.Left;
    const int height = m_Bounds.Bottom - m_Bounds.Top;
    m_Bounds = {x, y, x + width, y + height};
    m_HasBounds = true;
    ApplyBounds();
}

void WebViewState::SetVisible(bool visible)
{
    m_Visible = visible;
    m_HasVisibility = true;
    ApplyVisible();
}

void WebViewState::SetBackgroundColor(const WebViewColor& color)
{
    m_BackgroundColor = color;
    m_HasBackgroundColor = true;
    ApplyBackgroundColor();
}

void WebViewState::ApplyBounds()
{
    if (!m_Target || !m_HasBounds)
        return;
    if (m_HasAppliedBounds && m_AppliedBounds == m_Bounds)
        return;

    m_Target->ApplyBounds(m_Bounds);
    m_AppliedBounds = m_Bounds;
    m_HasAppliedBounds = true;
}

void WebViewState::ApplyVisible()
{
    if (!m_Target || !m_HasVisibility)
        return;
    if (m_HasAppliedVisibility && m_AppliedVisible == m_Visible)
        return;

    m_Target->ApplyVisible(m_Visible);
    m_AppliedVisible = m_Visible;
    m_HasAppliedVisibility = true;
}

void WebViewState::ApplyBackgroundColor()
{
    if (m_Target && m_HasBackgroundColor)
        m_Target->ApplyBackgroundColor(m_BackgroundColor);
}

} // namespace Platform
} // namespace GameEngine
