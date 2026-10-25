#include "WebViewGtkModule.h"
#include "WebViewState.h"

#include "Platform/WebView.h"

#include "Logger/Logger.h"

#include <algorithm>
#include <functional>
#include <string>

#include <gdk/gdkx.h>
#include <gtk/gtk.h>
#include <webkit2/webkit2.h>
#include <X11/Xlib.h>

namespace GameEngine {
namespace Platform {
namespace {

/// GTK events run per host frame, at most. A frame that leaves events waiting runs them next frame,
/// so a busy page cannot stall the editor.
constexpr int kMaxEventsPerFrame = 64;

enum class GtkStart {
    NotTried,
    Ready,
    Failed
};

GtkStart s_GtkStart = GtkStart::NotTried;

/// GTK starts on its X11 backend, on the display GLFW uses ($DISPLAY): on a Wayland desktop the
/// editor's window lives on XWayland, and only a window of the same X server can be its child.
bool StartGtk()
{
    if (s_GtkStart == GtkStart::NotTried)
    {
        gdk_set_allowed_backends("x11");
        s_GtkStart = gtk_init_check(nullptr, nullptr) ? GtkStart::Ready : GtkStart::Failed;
        if (s_GtkStart == GtkStart::Failed)
            Logger::Log::Error("WebView: GTK could not open the X11 display; the web view is not available");
    }
    return s_GtkStart == GtkStart::Ready;
}

/**
 * The Linux web view: a WebKitWebView in an undecorated GTK popup window whose X11 window is a child
 * of the host window.
 *
 * - The popup is override-redirect, so the window manager never manages it; X11 clips it to the host
 *   window and draws it above the GPU frame, like the macOS and Windows views.
 * - Bounds arrive in the host window's pixels. GTK positions and sizes windows in its own units,
 *   which are pixels divided by the GDK scale factor.
 * - The window manager gives keyboard focus to the host's top-level window only. A press in the page
 *   takes the X11 input focus for the popup, and ReturnFocus gives it back when the host sees a press
 *   outside the page.
 */
class WebView_Gtk : public IWebView, private IWebViewTarget {
public:
    explicit WebView_Gtk(unsigned long parentWindow)
        : m_ParentWindow(parentWindow)
    {
        m_Window = gtk_window_new(GTK_WINDOW_POPUP);
        m_WebView = WEBKIT_WEB_VIEW(webkit_web_view_new());
        gtk_container_add(GTK_CONTAINER(m_Window), GTK_WIDGET(m_WebView));
        gtk_widget_show(GTK_WIDGET(m_WebView));

        g_signal_connect(m_WebView, "button-press-event", G_CALLBACK(&WebView_Gtk::OnButtonPress), this);
        g_signal_connect(m_WebView, "load-changed", G_CALLBACK(&WebView_Gtk::OnLoadChanged), this);
        g_signal_connect(m_WebView, "load-failed", G_CALLBACK(&WebView_Gtk::OnLoadFailed), this);
        g_signal_connect(m_Window, "focus-out-event", G_CALLBACK(&WebView_Gtk::OnFocusOut), this);

        gtk_widget_realize(m_Window);
        GdkWindow* gdkWindow = gtk_widget_get_window(m_Window);
        XReparentWindow(XDisplay(), gdk_x11_window_get_xid(gdkWindow), m_ParentWindow, 0, 0);
        XFlush(XDisplay());

        m_State.Attach(*this);
    }

    ~WebView_Gtk() override { Destroy(); }

    bool LoadURL(const std::string& url) override { return m_Window && m_State.LoadURL(url); }

    bool LoadHTML(const std::string& html, const std::string& baseURL) override
    {
        if (!m_Window)
            return false;
        m_BaseURL = baseURL;
        return m_State.LoadHTML(html);
    }

    void GoBack() override
    {
        if (m_Window)
            webkit_web_view_go_back(m_WebView);
    }

    void GoForward() override
    {
        if (m_Window)
            webkit_web_view_go_forward(m_WebView);
    }

    void Reload() override
    {
        if (m_Window)
            webkit_web_view_reload(m_WebView);
    }

    void StopLoading() override
    {
        if (m_Window)
            webkit_web_view_stop_loading(m_WebView);
    }

    WebViewNavigation GetNavigation() const override
    {
        if (!m_Window)
            return {};
        const gchar* uri = webkit_web_view_get_uri(m_WebView);
        return {webkit_web_view_can_go_back(m_WebView) != FALSE, webkit_web_view_can_go_forward(m_WebView) != FALSE,
                webkit_web_view_is_loading(m_WebView) != FALSE, uri ? uri : ""};
    }

    void* GetNativeView() const override { return m_Window ? m_WebView : nullptr; }

    void Resize(int width, int height) override { m_State.Resize(width, height); }
    void SetPosition(int x, int y) override { m_State.SetPosition(x, y); }
    void SetVisible(bool visible) override { m_State.SetVisible(visible); }
    bool IsVisible() const override { return m_State.IsVisible(); }

    void SetBackgroundColor(unsigned char r, unsigned char g, unsigned char b) override
    {
        m_State.SetBackgroundColor({r, g, b});
    }

    bool BeginFileDrag(const std::filesystem::path& /*path*/) override { return false; }

    void SetOnMouseDown(std::function<void()> callback) override { m_OnMouseDown = std::move(callback); }

    // GTK creation is synchronous: a view that cannot start is never returned.
    void SetOnUnavailable(std::function<void(const std::string&)> /*callback*/) override {}

    void Destroy() override
    {
        if (!m_Window)
            return;

        if (s_FocusedView == this)
            s_FocusedView = nullptr;
        m_State.Detach();
        m_OnMouseDown = nullptr;
        g_signal_handlers_disconnect_by_data(m_WebView, this);
        g_signal_handlers_disconnect_by_data(m_Window, this);

        // The host window may already be gone, and X11 destroys a child window with its parent.
        GdkDisplay* display = gtk_widget_get_display(m_Window);
        gdk_x11_display_error_trap_push(display);
        gtk_widget_destroy(m_Window);
        gdk_x11_display_error_trap_pop_ignored(display);
        m_Window = nullptr;
        m_WebView = nullptr;
    }

    /// The host saw a press outside the page.
    static void ReturnFocus(unsigned long parentWindow)
    {
        if (!s_FocusedView || s_FocusedView->m_ParentWindow != parentWindow)
            return;

        s_FocusedView = nullptr;
        Display* display = XDisplay();
        XSetInputFocus(display, parentWindow, RevertToParent, CurrentTime);
        XFlush(display);
    }

private:
    static Display* XDisplay() { return gdk_x11_display_get_xdisplay(gdk_display_get_default()); }

    int ScaleFactor() const { return gtk_widget_get_scale_factor(m_Window); }

    void ApplyBounds(const WebViewBounds& bounds) override
    {
        const int scale = ScaleFactor();
        gtk_window_move(GTK_WINDOW(m_Window), bounds.Left / scale, bounds.Top / scale);
        gtk_window_resize(GTK_WINDOW(m_Window), std::max(1, (bounds.Right - bounds.Left) / scale),
                          std::max(1, (bounds.Bottom - bounds.Top) / scale));
    }

    void ApplyVisible(bool visible) override
    {
        if (visible)
            gtk_widget_show(m_Window);
        else
            gtk_widget_hide(m_Window);
    }

    void ApplyBackgroundColor(const WebViewColor& color) override
    {
        const GdkRGBA rgba{color.R / 255.0, color.G / 255.0, color.B / 255.0, 1.0};
        webkit_web_view_set_background_color(m_WebView, &rgba);
    }

    bool NavigateToURL(const std::string& url) override
    {
        webkit_web_view_load_uri(m_WebView, url.c_str());
        return true;
    }

    bool NavigateToHTML(const std::string& html) override
    {
        webkit_web_view_load_html(m_WebView, html.c_str(), m_BaseURL.empty() ? nullptr : m_BaseURL.c_str());
        return true;
    }

    /// Takes the keyboard focus for the page, then tells the host. Returns FALSE so the page also
    /// receives the press.
    static gboolean OnButtonPress(GtkWidget* /*widget*/, GdkEventButton* /*event*/, gpointer self)
    {
        auto* view = static_cast<WebView_Gtk*>(self);
        if (s_FocusedView != view)
        {
            Display* display = XDisplay();
            GdkDisplay* gdkDisplay = gtk_widget_get_display(view->m_Window);
            gdk_x11_display_error_trap_push(gdkDisplay);
            XSetInputFocus(display, gdk_x11_window_get_xid(gtk_widget_get_window(view->m_Window)), RevertToParent,
                           CurrentTime);
            gdk_x11_display_error_trap_pop_ignored(gdkDisplay);
            s_FocusedView = view;
        }
        gtk_widget_grab_focus(GTK_WIDGET(view->m_WebView));
        if (view->m_OnMouseDown)
            view->m_OnMouseDown();
        return FALSE;
    }

    static gboolean OnFocusOut(GtkWidget* /*widget*/, GdkEventFocus* /*event*/, gpointer self)
    {
        if (s_FocusedView == self)
            s_FocusedView = nullptr;
        return FALSE;
    }

    static void OnLoadChanged(WebKitWebView* /*webView*/, WebKitLoadEvent loadEvent, gpointer self)
    {
        auto* view = static_cast<WebView_Gtk*>(self);
        if (loadEvent == WEBKIT_LOAD_STARTED)
            view->m_LoadFailed = false;
        else if (loadEvent == WEBKIT_LOAD_FINISHED && !view->m_LoadFailed)
            Logger::Log::Info("WebView: page loaded");
    }

    static gboolean OnLoadFailed(WebKitWebView* /*webView*/, WebKitLoadEvent /*loadEvent*/, gchar* uri,
                                 GError* error, gpointer self)
    {
        static_cast<WebView_Gtk*>(self)->m_LoadFailed = true;
        // A load the user stopped, and a link that became a download, end here without failing.
        if (g_error_matches(error, WEBKIT_NETWORK_ERROR, WEBKIT_NETWORK_ERROR_CANCELLED) ||
            g_error_matches(error, WEBKIT_POLICY_ERROR, WEBKIT_POLICY_ERROR_FRAME_LOAD_INTERRUPTED_BY_POLICY_CHANGE))
            return FALSE;
        Logger::Log::Error("WebView: navigation to '{}' failed ({})", uri ? uri : "", error ? error->message : "");
        return FALSE;
    }

    /// The view that took the X11 input focus with a press in its page, until it loses it.
    static inline WebView_Gtk* s_FocusedView = nullptr;

    unsigned long m_ParentWindow = 0;
    GtkWidget* m_Window = nullptr;
    WebKitWebView* m_WebView = nullptr;
    WebViewState m_State;
    std::string m_BaseURL;
    std::function<void()> m_OnMouseDown;
    bool m_LoadFailed = false;
};

IWebView* CreateGtkWebView(unsigned long parentWindow)
{
    if (!StartGtk())
        return nullptr;
    return new WebView_Gtk(parentWindow);
}

void PumpGtkEvents()
{
    if (s_GtkStart != GtkStart::Ready)
        return;
    for (int i = 0; i < kMaxEventsPerFrame && g_main_context_iteration(nullptr, FALSE); ++i)
    {
    }
}

constexpr WebViewGtkModule kModule{&CreateGtkWebView, &PumpGtkEvents, &WebView_Gtk::ReturnFocus};

} // namespace
} // namespace Platform
} // namespace GameEngine

extern "C" __attribute__((visibility("default"))) const GameEngine::Platform::WebViewGtkModule* GeWebViewGtkModule()
{
    return &GameEngine::Platform::kModule;
}
