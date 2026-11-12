#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#if defined(__EMSCRIPTEN__)
#  include <emscripten/threading.h>
#elif defined(_WIN32)
#  include <windows.h>
#elif defined(__APPLE__)
#  include <pthread.h>
#elif defined(__linux__)
#  include <pthread.h>
#  include <sched.h>
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

// The processors this process may run on, as mask words: processor 0 is bit 0 of word 0.
// Empty when the read fails, when the process may use its whole processor group (Windows),
// or when the platform has no affinity API (macOS, the browser).
inline std::vector<std::uint64_t> GetAllowedProcessorMask()
{
#if defined(_WIN32)
    // The masks cover one processor group. They are equal when the process may use the whole
    // group, and both are 0 when the process has threads in more than one group.
    DWORD_PTR processMask = 0;
    DWORD_PTR systemMask = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask) == 0 || processMask == systemMask)
        return {};
    return {static_cast<std::uint64_t>(processMask)};
#elif defined(__linux__) && !defined(__EMSCRIPTEN__)
    // Process id 0 reads the mask of the calling thread, which the threads it starts inherit.
    // The read fails on a machine with more than CPU_SETSIZE processors.
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) != 0)
        return {};
    std::vector<std::uint64_t> mask(CPU_SETSIZE / 64, 0);
    for (int processor = 0; processor < CPU_SETSIZE; ++processor)
    {
        if (CPU_ISSET(processor, &set))
            mask[processor / 64] |= std::uint64_t{1} << (processor % 64);
    }
    return mask;
#else
    return {};
#endif
}

} // namespace GameEngine::Platform
