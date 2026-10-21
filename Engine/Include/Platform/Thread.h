#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

#if defined(__EMSCRIPTEN__)
#  include <emscripten/threading.h>
#elif defined(_WIN32)
#  include <windows.h>
#elif defined(__APPLE__)
#  include <pthread.h>
#elif defined(__linux__)
#  include <pthread.h>
#  include <cstring>
#endif

namespace GameEngine::Platform {

// Whether the calling thread can park while workers complete work. The
// browser main thread must service worker requests; native threads may block.
inline bool CanBlockCurrentThread()
{
#if defined(__EMSCRIPTEN__)
    return !emscripten_is_main_browser_thread();
#else
    return true;
#endif
}

// Set a descriptive name for the calling thread, visible in debuggers and profilers.
// Names should be short ASCII strings (Linux truncates to 15 characters).
inline void SetCurrentThreadName([[maybe_unused]] const char* name)
{
#if defined(_WIN32)
    if (!name) return;
    wchar_t wide[64];
    int i = 0;
    while (name[i] && i < 63)
    {
        wide[i] = static_cast<wchar_t>(name[i]);
        ++i;
    }
    wide[i] = L'\0';
    SetThreadDescription(GetCurrentThread(), wide);
#elif defined(__APPLE__)
    if (name) pthread_setname_np(name);
#elif defined(__linux__)
    if (name)
    {
        char truncated[16];
        std::strncpy(truncated, name, 15);
        truncated[15] = '\0';
        pthread_setname_np(pthread_self(), truncated);
    }
#endif
}

// The name a thread of this process was given (SetCurrentThreadName, or by the
// runtime), by its OS thread id: GetCurrentThreadId on Windows,
// pthread_threadid_np on macOS, gettid on Linux. Empty when the thread is gone,
// has no name, or the platform cannot read another thread's name.
std::string GetThreadName(std::uint64_t osThreadId);

// Blocks the calling thread for `duration`, overshooting by well under a
// millisecond. std::this_thread::sleep_for on Windows rounds up to the system
// timer tick (15.6 ms unless some process raised the resolution), so a 15 ms
// sleep can last 31 ms; a high-resolution waitable timer does not. Elsewhere
// the standard sleep already has that precision.
inline void SleepPrecise(std::chrono::duration<double> duration)
{
    if (duration.count() <= 0.0)
        return;
#if defined(_WIN32)
    struct TimerHandle
    {
        HANDLE Handle = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                               TIMER_ALL_ACCESS);
        ~TimerHandle()
        {
            if (Handle)
                CloseHandle(Handle);
        }
    };
    thread_local TimerHandle timer;
    if (timer.Handle)
    {
        // A negative due time is relative, in 100 ns units.
        LARGE_INTEGER dueTime{};
        dueTime.QuadPart = -static_cast<LONGLONG>(duration.count() * 1.0e7);
        if (SetWaitableTimer(timer.Handle, &dueTime, 0, nullptr, nullptr, FALSE))
        {
            WaitForSingleObject(timer.Handle, INFINITE);
            return;
        }
    }
#endif
    std::this_thread::sleep_for(duration);
}

} // namespace GameEngine::Platform
