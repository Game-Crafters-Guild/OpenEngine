#pragma once

#include <string>

namespace GameEngine {

class UIElement;

namespace UIUtil {

/// The path inside a CSS url(...) without an optional "asset:" scheme. A leading '/' stays;
/// the UI system does not allow that form.
std::string NormalizeCssUrlPath(std::string path);

/// Shows or hides `element` through the `hidden` class. The UI module's default sheet gives the
/// class `display: none`; the editor's core.css raises it over a control's own `display` rule.
void SetShown(UIElement& element, bool shown);

/// Adds `className` to `element` when `present` is true and removes it when it is false.
void SetClassPresent(UIElement& element, const std::string& className, bool present);

} // namespace UIUtil
} // namespace GameEngine
