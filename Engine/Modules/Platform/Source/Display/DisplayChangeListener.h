#pragma once

namespace GameEngine::Platform {

#if defined(_WIN32)
/// Class name of the listener's hidden window.
inline constexpr wchar_t kDisplayChangeListenerClassName[] = L"GameEngineDisplayChangeListener";
#endif

/// Calls a handler on the main thread, when that thread pumps its events, after the OS
/// reports a display change. Windows: a hidden top-level window gets WM_DISPLAYCHANGE (a
/// message-only window gets none). macOS: NSApplicationDidChangeScreenParametersNotification.
class DisplayChangeListener
{
public:
    using Handler = void (*)();

    DisplayChangeListener() = default;
    DisplayChangeListener(const DisplayChangeListener&) = delete;
    DisplayChangeListener& operator=(const DisplayChangeListener&) = delete;
    ~DisplayChangeListener();

    /// Starts to listen. Returns false and logs the error when the OS refuses
    /// the listener.
    bool Create(Handler handler);
    void Destroy();

private:
#if defined(_WIN32)
    void* m_Window = nullptr;
#elif defined(__APPLE__)
    void* m_Observer = nullptr;
#endif
};

} // namespace GameEngine::Platform
