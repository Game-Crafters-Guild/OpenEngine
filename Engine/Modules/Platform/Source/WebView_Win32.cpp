#if defined(_WIN32)

#include "Platform/WebView.h"
#include "Platform/Window.h"
#include "Utf8ToWide_Win32.h"
#include "WebViewEnvironment_Win32.h"
#include "WebViewState.h"

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include "Logger/Logger.h"

#include <windows.h>
#include <ole2.h> // WebView2.h needs the COM declarations WIN32_LEAN_AND_MEAN leaves out

#include <WebView2.h>
#include <wrl/client.h>
#include <wrl/event.h>

#include <string>

namespace GameEngine {
namespace Platform {

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

/**
 * The Windows web view: a WebView2 controller parented to the host window.
 *
 * - The controller is created asynchronously; WebViewState records every call made before it
 *   exists and applies them when it arrives.
 * - Completions run on the UI thread's message queue and hold the shared Handlers block, never the
 *   view: Destroy clears its owner, so a completion that arrives later does nothing.
 * - The view draws above the GPU frame, so a page that failed to load and a blank page look the
 *   same; the navigation outcome is logged.
 */
class WebView_Win32 : public IWebView, private IWebViewTarget {
public:
    WebView_Win32(HWND host, const std::filesystem::path& userDataFolder)
        : m_Host(host)
        , m_Handlers(std::make_shared<Handlers>())
    {
        m_Handlers->Owner = this;
        m_Environment = WebViewEnvironment_Win32::Acquire(userDataFolder);
        const std::shared_ptr<Handlers> handlers = m_Handlers;
        m_Environment->WhenCreated([handlers](ICoreWebView2Environment* environment, const std::string& failure) {
            if (handlers->Owner)
                handlers->Owner->OnEnvironmentCreated(environment, failure);
        });
    }

    ~WebView_Win32() override { Destroy(); }

    bool LoadURL(const std::string& url) override { return !m_Failed && m_State.LoadURL(url); }

    // NavigateToString takes no base URL; pages that need one arrive through LoadURL.
    bool LoadHTML(const std::string& html, const std::string& /*baseURL*/) override
    {
        return !m_Failed && m_State.LoadHTML(html);
    }

    void* GetNativeView() const override { return m_Destroyed ? nullptr : m_Host; }

    void Resize(int width, int height) override { m_State.Resize(width, height); }
    void SetPosition(int x, int y) override { m_State.SetPosition(x, y); }
    void SetVisible(bool visible) override { m_State.SetVisible(visible); }
    bool IsVisible() const override { return m_State.IsVisible(); }

    void SetBackgroundColor(unsigned char r, unsigned char g, unsigned char b) override
    {
        m_State.SetBackgroundColor({r, g, b});
    }

    bool BeginFileDrag(const std::filesystem::path& /*path*/) override { return false; }

    // Mouse-down callback not wired on this platform; see WebView.h.
    void SetOnMouseDown(std::function<void()> /*callback*/) override {}

    void SetOnUnavailable(std::function<void(const std::string&)> callback) override
    {
        m_OnUnavailable = std::move(callback);
        if (m_OnUnavailable && m_Failed)
            m_OnUnavailable(m_Failure);
    }

    void Destroy() override
    {
        if (m_Destroyed)
            return;
        m_Destroyed = true;

        m_Handlers->Owner = nullptr;
        m_State.Detach();
        m_OnUnavailable = nullptr;
        if (m_Controller)
        {
            m_WebView->remove_NavigationCompleted(m_NavigationCompletedToken);
            m_Controller->Close();
        }
        m_WebView = nullptr;
        m_Controller = nullptr;
        m_Environment.reset();
    }

private:
    struct Handlers {
        WebView_Win32* Owner = nullptr;
    };

    void OnEnvironmentCreated(ICoreWebView2Environment* environment, const std::string& failure)
    {
        if (!environment)
        {
            Fail(failure);
            return;
        }

        const std::shared_ptr<Handlers> handlers = m_Handlers;
        const HRESULT hr = environment->CreateCoreWebView2Controller(
            m_Host, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [handlers](HRESULT controllerResult, ICoreWebView2Controller* controller) -> HRESULT {
                            if (handlers->Owner)
                                handlers->Owner->OnControllerCreated(controllerResult, controller);
                            return S_OK;
                        })
                        .Get());
        if (FAILED(hr))
            FailStep("CreateCoreWebView2Controller", hr);
    }

    void OnControllerCreated(HRESULT result, ICoreWebView2Controller* controller)
    {
        if (FAILED(result) || !controller)
        {
            FailStep("the controller completion", result);
            return;
        }

        ComPtr<ICoreWebView2> webView;
        if (FAILED(controller->get_CoreWebView2(&webView)) || !webView)
        {
            controller->Close();
            FailStep("get_CoreWebView2", E_NOINTERFACE);
            return;
        }
        m_Controller = controller;
        m_WebView = webView;

        // The handler captures nothing, so it cannot outlive the view it reports for.
        m_WebView->add_NavigationCompleted(
            Callback<ICoreWebView2NavigationCompletedEventHandler>(
                [](ICoreWebView2* /*sender*/, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                    BOOL succeeded = FALSE;
                    args->get_IsSuccess(&succeeded);
                    if (succeeded)
                    {
                        Logger::Log::Info("WebView: page loaded");
                        return S_OK;
                    }
                    COREWEBVIEW2_WEB_ERROR_STATUS status = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
                    args->get_WebErrorStatus(&status);
                    Logger::Log::Error("WebView: navigation failed (WebErrorStatus {})", static_cast<int>(status));
                    return S_OK;
                })
                .Get(),
            &m_NavigationCompletedToken);

        m_State.Attach(*this);
    }

    void FailStep(const char* step, HRESULT hr)
    {
        Logger::Log::Error("WebView: {} failed (hr=0x{:08x})", step, static_cast<unsigned>(hr));
        Fail(WebViewEnvironment_Win32::RuntimeFailureMessage(hr));
    }

    // Every creation failure ends here, synchronous or not, so the caller hears of it once.
    void Fail(const std::string& failure)
    {
        m_Failed = true;
        m_Failure = failure;
        if (m_OnUnavailable)
            m_OnUnavailable(m_Failure);
    }

    void ApplyBounds(const WebViewBounds& bounds) override
    {
        m_Controller->put_Bounds(RECT{bounds.Left, bounds.Top, bounds.Right, bounds.Bottom});
    }

    void ApplyVisible(bool visible) override { m_Controller->put_IsVisible(visible ? TRUE : FALSE); }

    // The default background color arrived with the controller's second revision; an older
    // runtime keeps its white default.
    void ApplyBackgroundColor(const WebViewColor& color) override
    {
        ComPtr<ICoreWebView2Controller2> controller2;
        if (SUCCEEDED(m_Controller.As(&controller2)) && controller2)
            controller2->put_DefaultBackgroundColor(COREWEBVIEW2_COLOR{255, color.R, color.G, color.B});
    }

    bool NavigateToURL(const std::string& url) override
    {
        return SUCCEEDED(m_WebView->Navigate(Utf8ToWide(url).c_str()));
    }

    bool NavigateToHTML(const std::string& html) override
    {
        return SUCCEEDED(m_WebView->NavigateToString(Utf8ToWide(html).c_str()));
    }

    HWND m_Host = nullptr;
    std::shared_ptr<Handlers> m_Handlers;
    std::shared_ptr<WebViewEnvironment_Win32> m_Environment;
    ComPtr<ICoreWebView2Controller> m_Controller;
    ComPtr<ICoreWebView2> m_WebView;
    EventRegistrationToken m_NavigationCompletedToken{};
    WebViewState m_State;
    std::function<void(const std::string&)> m_OnUnavailable;
    std::string m_Failure;
    bool m_Failed = false;
    bool m_Destroyed = false;
};

std::unique_ptr<IWebView> CreateWebView(Window* parentWindow, const std::filesystem::path& userDataFolder)
{
    GLFWwindow* glfwWindow = parentWindow ? parentWindow->GetGLFWHandle() : nullptr;
    HWND host = glfwWindow ? glfwGetWin32Window(glfwWindow) : nullptr;
    if (!host)
        return nullptr;

    return std::make_unique<WebView_Win32>(host, userDataFolder);
}

} // namespace Platform
} // namespace GameEngine

#endif // _WIN32
