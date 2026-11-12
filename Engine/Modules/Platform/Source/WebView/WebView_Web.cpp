// WebView on web: a DOM <iframe> laid over the canvas.
//
// Every other backend composites an OS webview above the GPU surface. A canvas
// has no such layer, but the page it lives in does: the iframe is a sibling of
// the canvas, positioned in page coordinates over the rectangle the panel
// occupies. That is the whole design — the engine keeps drawing the panel's
// chrome, and the browser draws the page inside the hole.
//
// Coordinate space: the panel hands these calls CLIENT pixels, and emscripten's
// GLFW reports the canvas's CSS size as the window size (the backing store,
// canvas.width, is the framebuffer). Client pixels are therefore CSS pixels
// already, and no devicePixelRatio math belongs here — the one conversion is
// the canvas's own page offset, read per-apply because layout moves it.
//
// What a browser cannot give back: no cross-origin page will report its clicks,
// so SetOnMouseDown never fires (its contract names macOS and Linux only), and
// there is no OS drag source, so BeginFileDrag stays false.

#include "Platform/WebView.h"

#include <emscripten/em_js.h>

#include <cstdint>
#include <string>

// The JS side of WebViewWeb. Elements live in Module.__geWebViews keyed by the
// view's id. EM_JS bodies take named parameters: -Wpedantic refuses the
// dollar-sign argument names of EM_ASM.
// clang-format off
EM_JS(void, GeWebViewCreate, (std::uint32_t id), {
    Module.__geWebViews = Module.__geWebViews || {};
    var frame = document.createElement("iframe");
    frame.style.position = "absolute";
    frame.style.border = "0";
    frame.style.margin = "0";
    frame.style.padding = "0";
    // Above the canvas, below anything the shell puts on top of both.
    frame.style.zIndex = "10";
    frame.style.display = "none";
    // Under COEP a cross-origin frame loads only when it is credentialless; it loads without the user's sign-in.
    frame.setAttribute("credentialless", "");
    frame.setAttribute("allow", "autoplay; encrypted-media; picture-in-picture");
    document.body.appendChild(frame);
    Module.__geWebViews[id] = frame;
});

EM_JS(void, GeWebViewLoadUrl, (std::uint32_t id, const char* url), {
    var frame = (Module.__geWebViews || {})[id];
    if (!frame)
        return;
    // srcdoc outranks src: remove it so that a LoadURL after a LoadHTML navigates.
    frame.removeAttribute("srcdoc");
    frame.src = UTF8ToString(url);
});

EM_JS(void, GeWebViewLoadHtml, (std::uint32_t id, const char* html), {
    var frame = (Module.__geWebViews || {})[id];
    if (!frame)
        return;
    // Dropping src keeps the pair unambiguous in the other direction:
    // a frame told to show inline HTML must not race a still-loading
    // navigation from an earlier LoadURL.
    frame.removeAttribute("src");
    frame.srcdoc = UTF8ToString(html);
});

EM_JS(void, GeWebViewSetBackgroundColor, (std::uint32_t id, int r, int g, int b), {
    var frame = (Module.__geWebViews || {})[id];
    if (frame)
        frame.style.backgroundColor = "rgb(" + r + "," + g + "," + b + ")";
});

EM_JS(void, GeWebViewRemove, (std::uint32_t id), {
    var views = Module.__geWebViews || {};
    var frame = views[id];
    if (frame && frame.parentNode)
        frame.parentNode.removeChild(frame);
    delete views[id];
});

EM_JS(void, GeWebViewApply, (std::uint32_t id, int x, int y, int width, int height, int visible), {
    var frame = (Module.__geWebViews || {})[id];
    if (!frame)
        return;
    // The canvas's page offset is read on every apply: the shell is free
    // to lay the canvas out however it likes, and a cached offset would
    // leave the frame behind the first time the page reflows.
    var canvas = Module["canvas"] || document.querySelector("canvas");
    var originX = 0;
    var originY = 0;
    if (canvas)
    {
        var rect = canvas.getBoundingClientRect();
        originX = rect.left + window.scrollX;
        originY = rect.top + window.scrollY;
    }
    frame.style.left = (originX + x) + "px";
    frame.style.top = (originY + y) + "px";
    frame.style.width = width + "px";
    frame.style.height = height + "px";
    frame.style.display = visible ? "block" : "none";
});
// clang-format on

namespace GameEngine
{
namespace Platform
{
namespace
{

// Elements live in a JS-side registry keyed by this id rather than being
// addressed by a DOM id string, so two panels cannot collide on a name and
// GetNativeView has a stable non-null value to hand back.
std::uint32_t NextWebViewId()
{
    static std::uint32_t s_Next = 0;
    return ++s_Next;
}

class WebViewWeb final : public IWebView
{
  public:
    explicit WebViewWeb(std::uint32_t id) : m_Id(id) { GeWebViewCreate(m_Id); }

    ~WebViewWeb() override { Destroy(); }

    bool LoadURL(const std::string& url) override
    {
        if (m_Destroyed)
            return false;
        GeWebViewLoadUrl(m_Id, url.c_str());
        return true;
    }

    bool LoadHTML(const std::string& html, const std::string& /*baseURL*/) override
    {
        if (m_Destroyed)
            return false;
        // srcdoc rather than a blob URL: no object URL to leak, and the frame
        // is replaced atomically. The baseURL a caller passes cannot be honoured
        // — srcdoc content resolves relative URLs against this page — which only
        // affects relative links, and callers embed absolute ones.
        GeWebViewLoadHtml(m_Id, html.c_str());
        return true;
    }

    void* GetNativeView() const override
    {
        // No native view pointer exists, but callers use this as a liveness
        // check, so it answers with something stable and non-null while the
        // element is alive.
        return m_Destroyed ? nullptr
                           : reinterpret_cast<void*>(static_cast<std::uintptr_t>(m_Id));
    }

    void Resize(int width, int height) override
    {
        m_Width = width;
        m_Height = height;
        Apply();
    }

    void SetPosition(int x, int y) override
    {
        m_X = x;
        m_Y = y;
        Apply();
    }

    void SetVisible(bool visible) override
    {
        m_Visible = visible;
        Apply();
    }

    bool IsVisible() const override { return m_Visible; }

    void SetBackgroundColor(unsigned char r, unsigned char g, unsigned char b) override
    {
        if (m_Destroyed)
            return;
        GeWebViewSetBackgroundColor(m_Id, r, g, b);
    }

    bool BeginFileDrag(const std::filesystem::path& /*path*/) override { return false; }

    void SetOnMouseDown(std::function<void()> /*callback*/) override {}
    void SetOnUnavailable(std::function<void(const std::string&)> /*callback*/) override {}

    void Destroy() override
    {
        if (m_Destroyed)
            return;
        m_Destroyed = true;
        GeWebViewRemove(m_Id);
    }

  private:
    void Apply() const
    {
        if (m_Destroyed)
            return;
        GeWebViewApply(m_Id, m_X, m_Y, m_Width, m_Height, m_Visible ? 1 : 0);
    }

    std::uint32_t m_Id = 0;
    int m_X = 0;
    int m_Y = 0;
    int m_Width = 0;
    int m_Height = 0;
    bool m_Visible = false;
    bool m_Destroyed = false;
};

} // namespace

std::unique_ptr<IWebView> CreateWebView(Window* /*parentWindow*/, const std::filesystem::path& /*userDataFolder*/)
{
    return std::make_unique<WebViewWeb>(NextWebViewId());
}

} // namespace Platform
} // namespace GameEngine
