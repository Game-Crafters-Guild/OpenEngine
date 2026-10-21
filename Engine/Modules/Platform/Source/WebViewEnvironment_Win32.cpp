#if defined(_WIN32)

#include "WebViewEnvironment_Win32.h"

#include "Logger/Logger.h"

#include <delayimp.h>
#include <wrl/event.h>

#include <format>
#include <utility>

namespace GameEngine {
namespace Platform {

namespace {

// Engine.dll delay-loads the loader (Engine/CMakeLists.txt), so a process that never opens a web
// view starts without it. Resolving every import here, before the first call, turns a missing DLL
// into a creation failure instead of a delay-load exception.
constexpr char kLoaderDll[] = "WebView2Loader.dll";

constexpr char kRuntimeDownload[] = "https://go.microsoft.com/fwlink/p/?LinkId=2124703";

// The views hold the only strong references; this one never keeps an environment alive.
std::weak_ptr<WebViewEnvironment_Win32>& SharedEnvironment()
{
    static std::weak_ptr<WebViewEnvironment_Win32> environment;
    return environment;
}

} // namespace

std::shared_ptr<WebViewEnvironment_Win32> WebViewEnvironment_Win32::Acquire(const std::filesystem::path& userDataFolder)
{
    if (std::shared_ptr<WebViewEnvironment_Win32> existing = SharedEnvironment().lock())
        return existing;

    std::shared_ptr<WebViewEnvironment_Win32> environment(new WebViewEnvironment_Win32());
    SharedEnvironment() = environment;
    environment->Create(userDataFolder);
    return environment;
}

std::string WebViewEnvironment_Win32::RuntimeFailureMessage(HRESULT result)
{
    return std::format("The web view could not start (hr=0x{:08x}). Install the Microsoft Edge WebView2 Runtime "
                       "from {}, then open the view again.",
                       static_cast<unsigned>(result), kRuntimeDownload);
}

WebViewEnvironment_Win32::~WebViewEnvironment_Win32()
{
    m_Environment = nullptr;
    if (m_EnteredApartment)
        CoUninitialize();
}

void WebViewEnvironment_Win32::WhenCreated(Completion completion)
{
    if (!m_Finished)
    {
        m_Waiting.push_back(std::move(completion));
        return;
    }
    completion(m_Environment.Get(), m_Failure);
}

void WebViewEnvironment_Win32::Create(const std::filesystem::path& userDataFolder)
{
    // S_FALSE (the thread is already in a single-threaded apartment) is entered and left like S_OK.
    // RPC_E_CHANGED_MODE leaves the thread in a multi-threaded apartment, where the creation below
    // fails and reports it.
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    m_EnteredApartment = SUCCEEDED(apartment);
    if (apartment == RPC_E_CHANGED_MODE)
        Logger::Log::Warning("WebView: this thread is in a multi-threaded COM apartment; WebView2 needs a "
                             "single-threaded one");

    if (const HRESULT loaded = __HrLoadAllImportsForDll(kLoaderDll); FAILED(loaded))
    {
        m_Finished = true;
        m_Failure = std::format("{} could not be loaded (hr=0x{:08x}). The editor ships it beside its executable: "
                                "rebuild or reinstall the editor. Exported games do not ship it.",
                                kLoaderDll, static_cast<unsigned>(loaded));
        Logger::Log::Error("WebView: {}", m_Failure);
        return;
    }

    const std::weak_ptr<WebViewEnvironment_Win32> weakSelf = weak_from_this();
    const HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, userDataFolder.c_str(), nullptr,
        Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [weakSelf](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {
                if (const std::shared_ptr<WebViewEnvironment_Win32> self = weakSelf.lock())
                    self->Finish(result, environment, "the environment completion");
                return S_OK;
            })
            .Get());
    if (FAILED(hr))
        Finish(hr, nullptr, "CreateCoreWebView2EnvironmentWithOptions");
}

void WebViewEnvironment_Win32::Finish(HRESULT result, ICoreWebView2Environment* environment, const char* step)
{
    if (m_Finished)
        return;
    m_Finished = true;

    if (SUCCEEDED(result) && environment)
    {
        m_Environment = environment;
    }
    else
    {
        m_Failure = RuntimeFailureMessage(result);
        Logger::Log::Error("WebView: {} failed. {}", step, m_Failure);
    }

    std::vector<Completion> waiting = std::move(m_Waiting);
    m_Waiting.clear();
    for (Completion& completion : waiting)
        completion(m_Environment.Get(), m_Failure);
}

} // namespace Platform
} // namespace GameEngine

#endif // _WIN32
