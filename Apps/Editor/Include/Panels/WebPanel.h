#pragma once

#include "UI/Controls/DockPanel.h"

#include "Platform/WebView.h"
#include "UI/UIManagerRef.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine
{
struct AssetLoadHandle;
class UIElement;
class Button;
class TextField;
class Label;
namespace Platform
{
class Window;
}

// Web panel: loads any URL or YouTube in platform web views (WKWebView/WebView2/WebKitGTK),
// one view per tab.
class WebPanel : public DockPanel
{
  public:
    std::string_view DeclaredTabIconClass() const override { return "dock-web-icon"; }

    /// Engine documentation; also the panel's first-run start page (afterwards the
    /// last user-entered URL is restored from editor preferences).
    static constexpr const char* kHelpDocsURL = "https://www.openengine.dev/docs/";

    WebPanel();
    ~WebPanel() override;

    void OnPostLayout() override;

    static bool TryStartNativeAssetDrag(const std::filesystem::path& path);

    /// The host window the platform web views are children of. Setting it runs
    /// the setup that needs it, because the two arrive in either order: the
    /// panel binds (and tries to set up) when it is first shown, while
    /// ShowOrActivateWebPanel shows the panel and only then hands it the window.
    void SetWindow(Platform::Window* window);

    /// Show a URL without disturbing a page the user is browsing: activates an
    /// existing tab already on that URL, reuses a blank tab, or opens a new tab.
    /// Defers until the platform web views exist (created on first layout).
    void Navigate(const std::string& url);

    // When the panel is not the active dock tab, move the web views off-screen instead of
    // hiding them, so pages stay alive and audio keeps playing in the background.
    void SetWebViewOffScreen(bool offScreen);

  private:
    struct WebTab
    {
        std::unique_ptr<Platform::IWebView> view;
        std::string url;   // URL this tab last loaded; empty while blank
        std::string title; // short label shown in the tab bar
    };

    void BindFromAssetsDeferred();
    void LoadBindAttachLayoutAndStyle();
    void MarkBindFailed(std::string_view reason);
    void RefreshElementPointers();
    void OnURLChanged(const std::string& url);
    void OnLoadButtonClicked();
    void LoadURL(const std::string& url);
    void LoadYouTubeVideo(const std::string& embedURL);
    std::string ConvertToEmbedURL(const std::string& url);
    std::string ConvertEmbedToWatchURL(const std::string& embedURL);
    int ConvertTimestampToSeconds(const std::string& timestamp);
    void UpdateWebViewSize();
    void OnReloadClicked();
    void UpdateNavigationControls();
    void SetupWebView();

    WebTab* ActiveTab();
    Platform::IWebView* ActiveView();
    bool CreateTabWebView(WebTab& tab);
    void OnWebViewUnavailable(const std::string& message);
    void OpenBlankTab();
    void ActivateTab(size_t index);
    void CloseTab(size_t index);
    void RebuildTabBar();
    void MoveViewOffScreen(Platform::IWebView& view);

  private:
    // Deferred layout bind. m_BindPending covers the whole attempt — the queued action
    // AND the async asset loads it starts — so a bind in flight is not re-armed by every
    // layout pass. Every path that can end the attempt releases it: applied, failed,
    // "panel detached, try again", and the deferred action being dropped outright
    // (PostAction returning false, which is why OnPostLayout checks it).
    bool m_BindApplied = false;
    bool m_BindPending = false;
    bool m_BindFailed = false;
    bool m_WasVisible = true;

    // Stored so in-flight callbacks are cancelled if the panel is destroyed.
    std::unique_ptr<AssetLoadHandle> m_LayoutLoadHandle;
    std::unique_ptr<AssetLoadHandle> m_PanelStyleLoadHandle;

    UIElement* m_Container = nullptr;
    TextField* m_URLField = nullptr;
    Button* m_BackButton = nullptr;
    Button* m_ForwardButton = nullptr;
    Button* m_ReloadButton = nullptr;
    UIElement* m_TabBar = nullptr;
    UIElement* m_WebViewContainer = nullptr;
    Label* m_ErrorLabel = nullptr;

    std::vector<WebTab> m_Tabs;
    size_t m_ActiveTab = 0;
    Platform::Window* m_ParentWindow = nullptr;
    std::string m_PendingURL;
    // The active view's navigation state the controls last showed; empty until they first show one.
    std::optional<Platform::WebViewNavigation> m_ShownNavigation;
    // The page changes the state without a layout pass (a link, a load finishing), so the controls
    // also refresh on the manager's periodic tick.
    UIManagerRef m_RefreshManager;
    uint64_t m_RefreshToken = 0;
    // Why the web view cannot be created, stating the fix; empty while it can.
    std::string m_WebViewUnavailableMessage;
};

} // namespace GameEngine
