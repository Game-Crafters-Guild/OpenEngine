#pragma once

#include "Logger/Backtrace.h"
#include "Logger/LogChannel.h"
#include "Logger/LogLevel.h"
#include "Logger/LogSink.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <format>
#include <future>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace Logger
{

class Log
{
  public:
    /** Crash/terminate handlers call this first: symbolizing a backtrace
        walks dyld state that may be mid-teardown there, turning the crash
        report into a second crash. Errors logged afterwards carry no
        backtrace; the OS crash report has the real one. Irreversible. */
    static void SuppressBacktraceCapture();

    struct Config
    {
        LogLevel GlobalMinLevel = LogLevel::Info;
        bool EnableSourceLocation = false;
        // Route Warning and above to stderr on the default console sink. Correct for
        // a process whose stdout is consumed as data; false for one whose console is
        // read by a human, where splitting the severities across two independently
        // buffered streams costs ordering and loses half of any single-stream capture.
        bool ConsoleUseStderr = true;
    };

    static void Initialize(const Config& config);
    static void Shutdown();
    static void AddSink(std::unique_ptr<LogSink> sink);
    static void ClearSinks();
    static void SetLogLevel(LogLevel level);
    static LogLevel GetLogLevel();

    /// Sets the level of the channel at this LogChannel::Index. With nullopt, the channel uses the stricter of the
    /// global level and its default level again.
    /// The setting applies whatever the global level is, except while logging is off. Thread-safe.
    static void SetChannelLevel(std::uint16_t channel, std::optional<LogLevel> level);
    /// The setting SetChannelLevel stored, or nullopt when the channel follows the global level.
    static std::optional<LogLevel> GetChannelSetting(std::uint16_t channel);
    /// The level the channel passes now: its setting, else the stricter of the global and its default level.
    static LogLevel GetChannelLevel(std::uint16_t channel);
    /// The level the channel passes with no setting: the stricter of the global and its default level.
    static LogLevel GetChannelLevelWithoutSetting(std::uint16_t channel);
    /// The number of registered channels; channel indices run from 0 (Core) to this count less one.
    static std::uint16_t GetChannelCount();
    /// The registered name, or empty for an index that is not registered.
    static std::string_view GetChannelName(std::uint16_t channel);

    /**
     * @brief Block until every queued entry has reached the sinks and the sinks
     *        have been flushed.
     *
     * Unbounded by design: the wait ends only when the drain thread acknowledges,
     * and the drain thread cannot acknowledge while another thread holds the sink
     * lock. A caller that must make progress even then — anything on a path that
     * ends in abort(), raise() or a debug break — wants FlushForCrash instead.
     */
    static void Flush();

    /**
     * @brief Ask the drain thread for a flush, wait at most @p timeout, return
     *        either way.
     *
     * For death paths — anything that ends in abort(), raise() or a debug break.
     * It never takes the sink mutex and never waits on one without a deadline,
     * so a thread stuck or dead inside a sink cannot turn a crash into a hang.
     *
     * @return true if the logger acknowledged the flush (or had nothing to
     *         flush); false if the deadline passed first or no drain thread was
     *         left to answer, meaning the tail of the log may be missing.
     */
    static bool FlushForCrash(std::chrono::milliseconds timeout);

    static size_t GetSinkCount();

    // Cross-module unification. Logger is a STATIC library, so a host exe and
    // a shared engine DLL each get their own copy of the GetState() singleton
    // and their logs silently fork (DLL warnings never reach the host's ring/
    // file/panel sinks). The host adopts the engine's state at startup —
    //   Logger::Log::RedirectToSharedState(GameEngine::GetEngineLoggerState());
    // — after which every operation in the host's Logger copy (AddSink,
    // levels, the queue) acts on the shared state. Call once, on the main
    // thread, before Initialize/AddSink and before any worker threads log.
    struct LoggerState;
    static LoggerState* GetStateForSharing();
    static void RedirectToSharedState(LoggerState* state);

    // ========================================
    // MAIN LOGGING INTERFACE
    // ========================================

    static void Trace(std::string_view message) { if (ShouldLog(LogLevel::Trace)) EnqueueString(LogLevel::Trace, message); }

    template <typename... Args>
    static void Trace(std::format_string<Args...> fmt, Args&&... args)
    {
        if (!ShouldLog(LogLevel::Trace)) return;
        EnqueueFormatted(LogLevel::Trace, fmt.get(), std::make_format_args(args...));
    }

    static void Debug(std::string_view message) { if (ShouldLog(LogLevel::Debug)) EnqueueString(LogLevel::Debug, message); }

    template <typename... Args>
    static void Debug(std::format_string<Args...> fmt, Args&&... args)
    {
        if (!ShouldLog(LogLevel::Debug)) return;
        EnqueueFormatted(LogLevel::Debug, fmt.get(), std::make_format_args(args...));
    }

    static void Info(std::string_view message) { if (ShouldLog(LogLevel::Info)) EnqueueString(LogLevel::Info, message); }

    template <typename... Args>
    static void Info(std::format_string<Args...> fmt, Args&&... args)
    {
        if (!ShouldLog(LogLevel::Info)) return;
        EnqueueFormatted(LogLevel::Info, fmt.get(), std::make_format_args(args...));
    }

    static void Warning(std::string_view message) { if (ShouldLog(LogLevel::Warning)) EnqueueString(LogLevel::Warning, message); }

    template <typename... Args>
    static void Warning(std::format_string<Args...> fmt, Args&&... args)
    {
        if (!ShouldLog(LogLevel::Warning)) return;
        EnqueueFormatted(LogLevel::Warning, fmt.get(), std::make_format_args(args...));
    }

    static void Error(std::string_view message) { if (ShouldLog(LogLevel::Error)) EnqueueString(LogLevel::Error, message); }

    template <typename... Args>
    static void Error(std::format_string<Args...> fmt, Args&&... args)
    {
        if (!ShouldLog(LogLevel::Error)) return;
        EnqueueFormatted(LogLevel::Error, fmt.get(), std::make_format_args(args...));
    }

    // Critical formats eagerly and dispatches synchronously for crash safety.
    static void Critical(std::string_view message) { if (ShouldLog(LogLevel::Critical)) CriticalImpl(message); }

    template <typename... Args>
    static void Critical(std::format_string<Args...> fmt, Args&&... args)
    {
        if (!ShouldLog(LogLevel::Critical)) return;
        auto& buf = GetFormatBuffer();
        buf.clear();
        std::format_to(std::back_inserter(buf), fmt, std::forward<Args>(args)...);
        CriticalImpl(std::string_view(buf));
    }

    // The gate of the functions above: the Core channel's level. Below it nothing
    // is formatted, copied or queued.
    static bool ShouldLog(LogLevel level);

  private:

    // Backtrace capture must run on the calling thread (the drain thread's stack
    // is meaningless for diagnostics). Only Error+ severities pay the cost.
    static void MaybeAttachBacktrace(LogMessage& msg);

    // The single construction point for a queued record: everything but the
    // message text and the source, owned by the record before it leaves the producer.
    static LogMessage MakeOwnedRecord(LogLevel level);

    // Consume borrowed arguments and any module-defined formatter before the
    // producer returns. Only owned text/metadata crosses the asynchronous queue;
    // moving, dispatching or destroying a record never calls into its producer.
    static void EnqueueFormatted(LogLevel level, std::string_view format, std::format_args args);

    // String enqueue copies the supplied text into the owned record.
    static void EnqueueString(LogLevel level, std::string_view message);

    // Critical synchronous dispatch (formats eagerly, bypasses queue).
    static void CriticalImpl(std::string_view message);
    /// Sends a Critical record to the sinks on the calling thread.
    static void DispatchCritical(const LogMessage& msg);

    static void PushEntry(LogMessage&& entry);

    static const char* GetCachedThreadId();
    static std::string& GetFormatBuffer();

    // Async drain thread
    static void DrainThreadFunc();
    static void DispatchToSinks(const LogMessage& message);

  private:
    static LoggerState& GetState();

    friend LogChannel RegisterChannel(std::string_view name, LogLevel defaultLevel);
    friend bool Enabled(const LogChannel& channel, LogLevel level);
    friend void Enqueue(const LogChannel& channel, LogLevel level, const char* file, int line,
                        const char* function, std::string_view format, std::format_args args);

    Log(const Log&) = delete;
    Log& operator=(const Log&) = delete;
};

} // namespace Logger
