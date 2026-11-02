#include "DisplayChangeListener.h"
#include "Logger/Logger.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace GameEngine::Platform {

namespace {

LRESULT CALLBACK DisplayChangeListenerProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_DISPLAYCHANGE)
    {
        const auto handler = reinterpret_cast<DisplayChangeListener::Handler>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (handler)
            handler();
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

// The module that holds the window procedure (Engine.dll), so the class
// belongs to the module that unregisters it.
HINSTANCE ListenerModule()
{
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&DisplayChangeListenerProc), &module);
    return module;
}

} // namespace

DisplayChangeListener::~DisplayChangeListener()
{
    Destroy();
}

bool DisplayChangeListener::Create(Handler handler)
{
    if (m_Window)
        return true;

    const HINSTANCE module = ListenerModule();
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = &DisplayChangeListenerProc;
    windowClass.hInstance = module;
    windowClass.lpszClassName = kDisplayChangeListenerClassName;
    if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        Logger::Log::Error("Platform: the display-change listener class did not register (error {}); the monitor list is read again only when a monitor connects or disconnects",
                           GetLastError());
        return false;
    }

    HWND window = CreateWindowExW(WS_EX_TOOLWINDOW, kDisplayChangeListenerClassName, L"", WS_POPUP,
                                  0, 0, 0, 0, nullptr, nullptr, module, nullptr);
    if (!window)
    {
        Logger::Log::Error("Platform: the display-change listener window was not created (error {}); the monitor list is read again only when a monitor connects or disconnects",
                           GetLastError());
        return false;
    }
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(handler));
    m_Window = window;
    return true;
}

void DisplayChangeListener::Destroy()
{
    if (!m_Window)
        return;
    DestroyWindow(static_cast<HWND>(m_Window));
    m_Window = nullptr;
    UnregisterClassW(kDisplayChangeListenerClassName, ListenerModule());
}

} // namespace GameEngine::Platform
