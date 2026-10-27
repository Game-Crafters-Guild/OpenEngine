#pragma once

// The Logger module's private declarations. Its tests include them; nothing outside the module does.

#include "LogChannelTable.h"
#include "Logger/Logger.h"
#include "Logger/LogSink.h"

#include <atomic>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace Logger
{

// One logger: the sinks, the channel levels and the queue the drain thread empties. Tests hold its Mutex to
// keep the drain thread from dispatching.
struct Log::LoggerState
{
    Config m_Config;
    std::vector<std::unique_ptr<LogSink>> Sinks;
    std::mutex Mutex;
    std::atomic<bool> Initialized{false};
    /// The global level and every channel's level; the global level is Off until Initialize.
    LogChannelTable Channels;

    // Async drain infrastructure
    std::vector<LogMessage> Queue;
    std::mutex QueueMutex;
    std::condition_variable QueueCV;
    std::thread DrainThread;
    std::atomic<bool> DrainRunning{false};
    std::atomic<bool> DrainThreadWaiting{false};
    std::thread::id DrainThreadId{};
    std::vector<std::promise<void>> FlushPromises;

    ~LoggerState()
    {
        if (DrainThread.joinable())
        {
            DrainRunning.store(false, std::memory_order_release);
            QueueCV.notify_one();
            DrainThread.join();
        }
    }
};

/// Cuts the compiler's function text to the qualified name a record keeps: "Ns::Class::Name". The input is
/// MSVC's __FUNCTION__ or the __PRETTY_FUNCTION__ signature of clang and GCC. The anonymous namespace is dropped
/// on every compiler, so "(anonymous namespace)::Helper(int)" gives "Helper".
std::string QualifiedFunctionName(std::string_view signature);

} // namespace Logger
