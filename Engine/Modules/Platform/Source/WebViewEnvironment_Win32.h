#pragma once

#if defined(_WIN32)

#include <windows.h>
#include <ole2.h> // WebView2.h needs the COM declarations WIN32_LEAN_AND_MEAN leaves out

#include <WebView2.h>
#include <wrl/client.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace GameEngine {
namespace Platform {

/**
 * @brief The process's WebView2 environment, shared by every web view on the UI thread.
 *
 * One environment is one browser process; every controller created from it shares that process
 * and its profile. Views hold the environment through Acquire: the first acquisition creates it,
 * the last holder's release closes it, and an acquisition after that creates a new one. The
 * profile folder is the one the creating acquisition names.
 *
 * WebView2 runs in a single-threaded apartment. The environment enters the calling thread's
 * apartment when it is created and leaves it when it is destroyed, so it must be acquired and
 * released on that one thread. Creation completes through the thread's message queue, which the
 * window loop pumps (Platform::Window::PollEvents).
 */
class WebViewEnvironment_Win32 : public std::enable_shared_from_this<WebViewEnvironment_Win32> {
public:
    /// Receives the environment, or nullptr and a message for the user that states the fix.
    using Completion = std::function<void(ICoreWebView2Environment* environment, const std::string& failure)>;

    static std::shared_ptr<WebViewEnvironment_Win32> Acquire(const std::filesystem::path& userDataFolder);

    ~WebViewEnvironment_Win32();

    WebViewEnvironment_Win32(const WebViewEnvironment_Win32&) = delete;
    WebViewEnvironment_Win32& operator=(const WebViewEnvironment_Win32&) = delete;

    /// The message for the user when WebView2 fails with `result`: the runtime to install.
    static std::string RuntimeFailureMessage(HRESULT result);

    /// Runs `completion` at once when creation has finished, else when it finishes.
    void WhenCreated(Completion completion);

private:
    WebViewEnvironment_Win32() = default;

    void Create(const std::filesystem::path& userDataFolder);
    void Finish(HRESULT result, ICoreWebView2Environment* environment, const char* step);

    Microsoft::WRL::ComPtr<ICoreWebView2Environment> m_Environment;
    std::vector<Completion> m_Waiting;
    std::string m_Failure;
    bool m_Finished = false;
    bool m_EnteredApartment = false;
};

} // namespace Platform
} // namespace GameEngine

#endif // _WIN32
