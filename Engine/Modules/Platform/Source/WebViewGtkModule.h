#pragma once

#include <memory>

namespace GameEngine {
namespace Platform {

class IWebView;

/**
 * @brief The entry points of the Linux web view module (PlatformWebViewGtk), which WebView_Linux.cpp
 * loads at run time.
 *
 * The module holds every WebKitGTK and GTK call, so libEngine.so, and the Player with it, never
 * depends on WebKitGTK. Every entry point runs on the thread that pumps the host window's events.
 * X11 window ids are passed as unsigned long, the type Xlib defines its Window as.
 */
struct WebViewGtkModule {
    /// A web view whose window is a child of the X11 window parentWindow, or nullptr when GTK cannot
    /// start on the X11 display.
    std::unique_ptr<IWebView> (*Create)(unsigned long parentWindow);

    /// Runs the GTK events that are ready, without waiting. Called once per host frame.
    void (*PumpEvents)();

    /// The host saw a mouse press in parentWindow outside every web view: a web view of that window
    /// that holds the keyboard focus gives it back to parentWindow.
    void (*ReturnFocus)(unsigned long parentWindow);
};

/// The name of the module's one exported symbol, GeWebViewGtkModule.
inline constexpr const char* kWebViewGtkModuleEntry = "GeWebViewGtkModule";

} // namespace Platform
} // namespace GameEngine

extern "C" const GameEngine::Platform::WebViewGtkModule* GeWebViewGtkModule();
