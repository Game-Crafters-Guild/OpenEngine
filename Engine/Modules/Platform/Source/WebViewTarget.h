#pragma once

#include <cstdint>
#include <string>

namespace GameEngine {
namespace Platform {

/// A web view's rectangle in the host window's client pixels; right and bottom are exclusive.
struct WebViewBounds {
    int Left = 0;
    int Top = 0;
    int Right = 0;
    int Bottom = 0;

    bool operator==(const WebViewBounds&) const = default;
};

struct WebViewColor {
    uint8_t R = 0;
    uint8_t G = 0;
    uint8_t B = 0;

    bool operator==(const WebViewColor&) const = default;
};

/// The native view a WebViewState drives once it exists.
class IWebViewTarget {
public:
    virtual ~IWebViewTarget() = default;

    virtual void ApplyBounds(const WebViewBounds& bounds) = 0;
    virtual void ApplyVisible(bool visible) = 0;
    virtual void ApplyBackgroundColor(const WebViewColor& color) = 0;
    virtual bool NavigateToURL(const std::string& url) = 0;
    virtual bool NavigateToHTML(const std::string& html) = 0;
};

} // namespace Platform
} // namespace GameEngine
