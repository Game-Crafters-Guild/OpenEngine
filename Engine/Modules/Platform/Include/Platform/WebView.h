#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace GameEngine {
namespace Platform {

class Window;

/// What a web view's navigation controls show: the history around the current page, whether a page
/// is loading, and the page's URL.
struct WebViewNavigation {
    bool CanGoBack = false;
    bool CanGoForward = false;
    bool IsLoading = false;
    /// The URL of the page on screen, which links and redirects change; empty where the platform
    /// cannot read it.
    std::string URL;

    bool operator==(const WebViewNavigation&) const = default;
};

// Cross-platform web view interface
// Provides a unified API for embedding web content (YouTube, etc.) in the editor
class IWebView {
public:
    virtual ~IWebView() = default;

    // Load a URL in the web view
    virtual bool LoadURL(const std::string& url) = 0;

    // Load HTML content directly (for better control over iframe embeds)
    virtual bool LoadHTML(const std::string& html, const std::string& baseURL = "https://www.youtube.com") = 0;

    // Move through the view's history. Each does nothing when GetNavigation reports it cannot.
    virtual void GoBack() = 0;
    virtual void GoForward() = 0;

    // Load the current page again, or stop the page that is loading.
    virtual void Reload() = 0;
    virtual void StopLoading() = 0;

    // The view's navigation state, cheap enough to read every frame. A view whose native view does not
    // exist yet reports the defaults.
    // Web: an embedded page of another site hides its history, loading state and URL, so only Reload
    //   works there.
    virtual WebViewNavigation GetNavigation() const = 0;

    // The native handle the view lives in, or nullptr once it is destroyed; callers use it only as
    // "the view exists".
    // macOS: the WKWebView (NSView*)
    // Windows: the host window (HWND) the WebView2 controller is parented to, also while creation is
    //   pending or has failed (SetOnUnavailable reports a failure)
    // Linux: the WebKitWebView (GtkWidget*)
    // Web: a stable non-null token while the element exists
    virtual void* GetNativeView() const = 0;

    // Resize the web view to the given dimensions (in pixels)
    virtual void Resize(int width, int height) = 0;

    // Set the position of the web view relative to its parent (in pixels)
    virtual void SetPosition(int x, int y) = 0;

    // Show or hide the web view
    virtual void SetVisible(bool visible) = 0;

    // Check if the web view is visible
    virtual bool IsVisible() const = 0;

    // Set the background color (RGB values 0-255)
    virtual void SetBackgroundColor(unsigned char r, unsigned char g, unsigned char b) = 0;

    // Start a native OS file drag from the host window so embedded web content can
    // receive a real HTML5 file drop. Returns false when unsupported on the platform.
    virtual bool BeginFileDrag(const std::filesystem::path& path) = 0;

    // Invoked on mouse-down inside the web view. Native web-view clicks never reach
    // the engine input path, so the host UI uses this to clear its own focus state
    // (e.g. a focused URL field). Fired on macOS and Linux.
    virtual void SetOnMouseDown(std::function<void()> callback) = 0;

    // Invoked once if the view cannot be created, with a message for the user that states the
    // fix (for example the runtime to install). A view whose creation has already failed invokes
    // it at registration. Fired on Windows only: elsewhere CreateWebView returns nullptr instead.
    virtual void SetOnUnavailable(std::function<void(const std::string& message)> callback) = 0;

    // Destroy the web view and clean up resources
    virtual void Destroy() = 0;
};

// Factory function to create a platform-specific web view
// parentWindow: The GLFW window that will host the web view
// userDataFolder: a per-user folder the process can write, where the view keeps its profile
//   (cookies, cache); the caller names it, since only the application knows its own paths. The
//   profile is browser cache, so it belongs under local (not roaming) application data.
//   Windows only: macOS and Linux keep the system's default website data store.
// Returns nullptr if web view creation fails or is not supported on this platform
std::unique_ptr<IWebView> CreateWebView(Window* parentWindow, const std::filesystem::path& userDataFolder);

} // namespace Platform
} // namespace GameEngine
