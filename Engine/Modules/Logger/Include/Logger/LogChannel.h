#pragma once

#include "Logger/LogLevel.h"

#include <cstdint>
#include <format>
#include <string_view>

namespace Logger
{

/// A named log channel: each record carries one, and the Log panel filters and sets levels by it.
/// Declare it with GE_LOG_CHANNEL in its module's header and log on it with GE_LOG
/// (docs/scripting/native-logging.mdx).
struct LogChannel
{
    /// The name the sinks and the Log panel show.
    std::string_view Name;
    /// The lowest level the channel passes while no setting overrides it, also when the global level is lower.
    LogLevel DefaultLevel = LogLevel::Info;
    /// The channel's slot in the logger's channel table.
    std::uint16_t Index = 0;
};

/// The number of channels one process can register: about four for each engine module, with room for the
/// game and its packages. The level table stays at 256 bytes.
inline constexpr std::uint16_t kMaxLogChannels = 256;

/// GE_LOG removes Trace and Debug records from builds that define NDEBUG.
#if defined(NDEBUG)
inline constexpr LogLevel kCompiledMinLogLevel = LogLevel::Info;
#else
inline constexpr LogLevel kCompiledMinLogLevel = LogLevel::Trace;
#endif

/// Registers a channel, or returns the channel already registered under this name with its first default
/// level. Thread-safe and allowed before Initialize. Past kMaxLogChannels it asserts and returns Core.
LogChannel RegisterChannel(std::string_view name, LogLevel defaultLevel);

/// True when a record of this level on this channel reaches the sinks. Lock-free: one byte load.
bool Enabled(const LogChannel& channel, LogLevel level);

/// Formats one record and queues it; a Critical record reaches the sinks before the call returns.
/// `file` and `function` are the caller's literals; the record copies them before the call returns.
void Enqueue(const LogChannel& channel, LogLevel level, const char* file, int line, const char* function,
             std::string_view format, std::format_args args);

/// The channel of every record that names no channel: the Log:: functions and the LOG_ macros.
inline const LogChannel& CoreLog()
{
    static constexpr LogChannel kCore{"Core", LogLevel::Trace, 0};
    return kCore;
}

/// The step of GE_LOG that checks the format string at compile time and erases the argument types.
template <typename... Args>
void Write(const LogChannel& channel, LogLevel level, const char* file, int line, const char* function,
           std::format_string<Args...> format, Args&&... args)
{
    Enqueue(channel, level, file, line, function, format.get(), std::make_format_args(args...));
}

} // namespace Logger

/// The compiler's function text that GE_LOG passes. The logger cuts it to the qualified name on every compiler.
#if defined(_MSC_VER)
#define GE_LOG_FUNCTION __FUNCTION__
#else
#define GE_LOG_FUNCTION __PRETTY_FUNCTION__
#endif

/// Defines `inline const Logger::LogChannel& Symbol()`, which registers the channel on its first call. Use it
/// once, in the owning module's header. Each DLL registers the name again and gets the same channel.
/// Level has no scope: GE_LOG_CHANNEL(RenderingLog, "Rendering", Info).
#define GE_LOG_CHANNEL(Symbol, NameLiteral, Level)                                                                 \
    inline const ::Logger::LogChannel& Symbol()                                                                    \
    {                                                                                                              \
        static const ::Logger::LogChannel channel = ::Logger::RegisterChannel(NameLiteral, ::Logger::LogLevel::Level); \
        return channel;                                                                                            \
    }

/// Logs one record on a channel with the call site's file, line and function. Nothing is evaluated when the
/// channel does not pass the level: GE_LOG(RenderingLog, Warning, "lost {} frames", frames).
#define GE_LOG(Channel, Level, Format, ...)                                                                        \
    do                                                                                                             \
    {                                                                                                              \
        if constexpr (::Logger::LogLevel::Level >= ::Logger::kCompiledMinLogLevel)                                 \
        {                                                                                                          \
            const ::Logger::LogChannel& loggerChannel = Channel();                                                 \
            if (::Logger::Enabled(loggerChannel, ::Logger::LogLevel::Level))                                       \
                ::Logger::Write(loggerChannel, ::Logger::LogLevel::Level, __FILE__, __LINE__, GE_LOG_FUNCTION,     \
                                Format __VA_OPT__(, ) __VA_ARGS__);                                                \
        }                                                                                                          \
    } while (false)

// ========================================
// CONVENIENCE MACROS
// ========================================

// The Core channel with the call site's file, line and function; GE_LOG removes Trace and Debug where
// NDEBUG is defined.
#define LOG_TRACE(format, ...) GE_LOG(::Logger::CoreLog, Trace, format __VA_OPT__(,) __VA_ARGS__)
#define LOG_DEBUG(format, ...) GE_LOG(::Logger::CoreLog, Debug, format __VA_OPT__(,) __VA_ARGS__)
#define LOG_INFO(format, ...) GE_LOG(::Logger::CoreLog, Info, format __VA_OPT__(,) __VA_ARGS__)
#define LOG_WARNING(format, ...) GE_LOG(::Logger::CoreLog, Warning, format __VA_OPT__(,) __VA_ARGS__)
#define LOG_ERROR(format, ...) GE_LOG(::Logger::CoreLog, Error, format __VA_OPT__(,) __VA_ARGS__)
#define LOG_CRITICAL(format, ...) GE_LOG(::Logger::CoreLog, Critical, format __VA_OPT__(,) __VA_ARGS__)
