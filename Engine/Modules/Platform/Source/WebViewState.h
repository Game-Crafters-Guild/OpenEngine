#pragma once

#include "WebViewTarget.h"

#include <string>

namespace GameEngine {
namespace Platform {

/**
 * @brief What a web view has been asked to show, for a native view that is created asynchronously.
 *
 * Calls made before a target is attached are recorded and applied by Attach, in the order that
 * keeps the view quiet when it appears: background color, bounds, visibility, then the page. Only
 * the last page requested before attachment loads. Once attached, a call reaches the target only
 * when it changes what the target last received: the panel re-issues size, position and
 * visibility every frame, and each one the target receives is a call into the browser process.
 *
 * Resize and SetPosition each change one half of the rectangle and keep the other, so the pair
 * the panel sends never puts the view at the window's origin in between.
 */
class WebViewState {
public:
    void Attach(IWebViewTarget& target);
    void Detach();

    bool LoadURL(const std::string& url);
    bool LoadHTML(const std::string& html);
    void Resize(int width, int height);
    void SetPosition(int x, int y);
    void SetVisible(bool visible);
    bool IsVisible() const { return m_Visible; }
    void SetBackgroundColor(const WebViewColor& color);

private:
    enum class Page {
        None,
        URL,
        HTML
    };

    void ApplyBounds();
    void ApplyVisible();
    void ApplyBackgroundColor();

    IWebViewTarget* m_Target = nullptr;
    WebViewBounds m_Bounds;
    WebViewBounds m_AppliedBounds;
    WebViewColor m_BackgroundColor;
    std::string m_PendingPage;
    Page m_PendingKind = Page::None;
    bool m_HasBounds = false;
    bool m_HasAppliedBounds = false;
    bool m_HasVisibility = false;
    bool m_Visible = false;
    bool m_HasAppliedVisibility = false;
    bool m_AppliedVisible = false;
    bool m_HasBackgroundColor = false;
};

} // namespace Platform
} // namespace GameEngine
