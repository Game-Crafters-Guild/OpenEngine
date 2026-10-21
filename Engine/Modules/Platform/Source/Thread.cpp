#include "Platform/Thread.h"

#if defined(__APPLE__)
#  include <libproc.h>
#  include <sys/proc_info.h>
#  include <unistd.h>
#  include <cstring>
#elif defined(__linux__)
#  include <cstdio>
#endif

namespace GameEngine::Platform {

std::string GetThreadName([[maybe_unused]] std::uint64_t osThreadId)
{
#if defined(_WIN32)
    std::string name;
    HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(osThreadId));
    if (!thread) return name;
    PWSTR wide = nullptr;
    if (SUCCEEDED(GetThreadDescription(thread, &wide)) && wide)
    {
        for (const wchar_t* c = wide; *c; ++c)
            name.push_back(*c < 0x80 ? static_cast<char>(*c) : '?');
        LocalFree(wide);
    }
    CloseHandle(thread);
    return name;
#elif defined(__APPLE__)
    proc_threadinfo info{};
    if (proc_pidinfo(getpid(), PROC_PIDTHREADID64INFO, osThreadId, &info, sizeof(info)) != sizeof(info))
        return {};
    return std::string(info.pth_name, strnlen(info.pth_name, sizeof(info.pth_name)));
#elif defined(__linux__)
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/self/task/%llu/comm", static_cast<unsigned long long>(osThreadId));
    std::FILE* file = std::fopen(path, "r");
    if (!file) return {};
    char buffer[32] = {};
    const std::size_t read = std::fread(buffer, 1, sizeof(buffer) - 1, file);
    std::fclose(file);
    std::string name(buffer, read);
    while (!name.empty() && name.back() == '\n') name.pop_back();
    return name;
#else
    return {};
#endif
}

} // namespace GameEngine::Platform
