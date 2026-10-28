#pragma once

struct GLFWwindow;

namespace GameEngine {
namespace Platform {

/// Runs the web views' GTK events. Window::PollEvents calls it once per frame; it does nothing until
/// a web view exists.
void PumpWebViewEvents();

/// Window's mouse-button handler calls it on a press in host: a web view of host that holds the
/// keyboard focus gives it back, because X11 gives the focus to the window it is set on, not the
/// window clicked.
void ReturnWebViewFocus(GLFWwindow* host);

} // namespace Platform
} // namespace GameEngine
