#if !defined(_WIN32) && !defined(__APPLE__)

#include "Platform/WebView.h"
#include "Platform/Window.h"
#include "WebViewGtkModule.h"
#include "WebViewHost_Linux.h"

#include "Logger/Logger.h"

#include <filesystem>

#include <dlfcn.h>
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_X11
#include <GLFW/glfw3native.h>

namespace GameEngine {
namespace Platform {
namespace {

#if GE_HAVE_WEBKITGTK

const WebViewGtkModule* s_Module = nullptr;
bool s_ModuleLoadTried = false;

/// The module lives beside libEngine.so in the editor's output folder, and nowhere a Player is
/// staged, so a game never loads WebKitGTK.
std::filesystem::path ModulePath()
{
    Dl_info info{};
    if (!dladdr(reinterpret_cast<void*>(&ModulePath), &info) || !info.dli_fname)
        return GE_WEBVIEW_GTK_MODULE;
    return std::filesystem::path(info.dli_fname).parent_path() / GE_WEBVIEW_GTK_MODULE;
}

/// Loads the module once. It stays loaded for the life of the process: views it created may be
/// alive at any time, and GTK cannot be unloaded.
const WebViewGtkModule* LoadModule()
{
    if (s_ModuleLoadTried)
        return s_Module;
    s_ModuleLoadTried = true;

    const std::filesystem::path path = ModulePath();
    void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL | RTLD_NODELETE);
    if (!handle)
    {
        Logger::Log::Error("WebView: {} could not be loaded ({}). Install WebKitGTK 4.1 (webkit2gtk4.1 on "
                           "Fedora, libwebkit2gtk-4.1-0 on Debian and Ubuntu), then open the view again.",
                           path.string(), dlerror());
        return nullptr;
    }

    using EntryFunction = const WebViewGtkModule* (*)();
    auto entry = reinterpret_cast<EntryFunction>(dlsym(handle, kWebViewGtkModuleEntry));
    if (!entry)
    {
        Logger::Log::Error("WebView: {} has no {} entry point", path.string(), kWebViewGtkModuleEntry);
        return nullptr;
    }
    s_Module = entry();
    return s_Module;
}

#endif // GE_HAVE_WEBKITGTK

} // namespace

std::unique_ptr<IWebView> CreateWebView(Window* parentWindow, const std::filesystem::path& /*userDataFolder*/)
{
#if GE_HAVE_WEBKITGTK
    GLFWwindow* glfwWindow = parentWindow ? parentWindow->GetGLFWHandle() : nullptr;
    if (!glfwWindow)
        return nullptr;

    // Only an X11 window can take the view's window as a child.
    if (glfwGetPlatform() != GLFW_PLATFORM_X11)
    {
        Logger::Log::Warning("WebView: the web view needs an X11 window; this window system is not supported");
        return nullptr;
    }

    const WebViewGtkModule* module = LoadModule();
    if (!module)
        return nullptr;
    return std::unique_ptr<IWebView>(module->Create(glfwGetX11Window(glfwWindow)));
#else
    (void)parentWindow;
    return nullptr;
#endif
}

void PumpWebViewEvents()
{
#if GE_HAVE_WEBKITGTK
    if (s_Module)
        s_Module->PumpEvents();
#endif
}

void ReturnWebViewFocus(GLFWwindow* host)
{
#if GE_HAVE_WEBKITGTK
    if (s_Module)
        s_Module->ReturnFocus(glfwGetX11Window(host));
#else
    (void)host;
#endif
}

} // namespace Platform
} // namespace GameEngine

#endif // !_WIN32 && !__APPLE__
