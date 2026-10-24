#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#if defined(_WIN32) || defined(_WIN64)
// Windows: no unistd.h, we avoid isatty/fileno
#else
#include <unistd.h>
#endif

namespace Logger {

/**
 * @brief Log levels for filtering and categorizing log messages
 */
enum class LogLevel : std::uint8_t {
    Trace = 0,      // Detailed trace information (very verbose)
    Debug = 1,      // Debug information (verbose)
    Info = 2,       // General information (normal)
    Warning = 3,    // Warning conditions (important)
    Error = 4,      // Error conditions (critical)
    Critical = 5,   // Critical error conditions (fatal)
    Off = 255       // Disable all logging
};

/**
 * @brief Convert LogLevel to string representation
 * @param level Log level to convert
 * @return String representation of the log level
 */
inline const char* LogLevelToString(LogLevel level) {
    switch (level) {
        case LogLevel::Trace:    return "TRACE";
        case LogLevel::Debug:    return "DEBUG";
        case LogLevel::Info:     return "INFO";
        case LogLevel::Warning:  return "WARN";
        case LogLevel::Error:    return "ERROR";
        case LogLevel::Critical: return "CRITICAL";
        case LogLevel::Off:      return "OFF";
        default:                 return "UNKNOWN";
    }
}

/**
 * @brief Convert LogLevel to short string representation (for compact output)
 * @param level Log level to convert
 * @return Short string representation of the log level
 */
inline const char* LogLevelToShortString(LogLevel level) {
    switch (level) {
        case LogLevel::Trace:    return "T";
        case LogLevel::Debug:    return "D";
        case LogLevel::Info:     return "I";
        case LogLevel::Warning:  return "W";
        case LogLevel::Error:    return "E";
        case LogLevel::Critical: return "C";
        case LogLevel::Off:      return "X";
        default:                 return "?";
    }
}

/**
 * @brief Parse a log level name
 * @param name trace, debug, info, warning (or warn), error, critical or off, in
 *        lower or upper case
 * @return The level, or nullopt for any other spelling, so a caller can refuse a
 *         typo instead of logging at a level nobody asked for
 */
inline std::optional<LogLevel> ParseLogLevel(std::string_view name) {
    if (name == "TRACE" || name == "trace") { return LogLevel::Trace; }
    if (name == "DEBUG" || name == "debug") { return LogLevel::Debug; }
    if (name == "INFO" || name == "info") { return LogLevel::Info; }
    if (name == "WARNING" || name == "WARN" || name == "warning" || name == "warn") { return LogLevel::Warning; }
    if (name == "ERROR" || name == "error") { return LogLevel::Error; }
    if (name == "CRITICAL" || name == "critical") { return LogLevel::Critical; }
    if (name == "OFF" || name == "off") { return LogLevel::Off; }
    return std::nullopt;
}

/**
 * @brief Check if a log level should be logged given the minimum level
 * @param level Log level to check
 * @param minLevel Minimum log level threshold
 * @return True if the level should be logged
 */
inline bool ShouldLog(LogLevel level, LogLevel minLevel) {
    return level >= minLevel && minLevel != LogLevel::Off;
}

/**
 * @brief Get ANSI color code for log level (for colored console output)
 * @param level Log level
 * @return ANSI color code string
 */
inline const char* LogLevelToColorCode(LogLevel level) {
    switch (level) {
        case LogLevel::Trace:    return "\033[37m";   // White
        case LogLevel::Debug:    return "\033[36m";   // Cyan
        case LogLevel::Info:     return "\033[32m";   // Green
        case LogLevel::Warning:  return "\033[33m";   // Yellow
        case LogLevel::Error:    return "\033[31m";   // Red
        case LogLevel::Critical: return "\033[35m";   // Magenta
        default:                 return "\033[0m";    // Reset
    }
}

/**
 * @brief ANSI reset color code
 */
inline const char* GetColorReset() {
    return "\033[0m";
}

/**
 * @brief Check if colored output is supported/enabled
 * @return True if colored output should be used
 */
inline bool IsColorOutputSupported() {
#if defined(_WIN32) || defined(_WIN64)
    // Windows 10 version 1607 and later support ANSI escape sequences
    // For simplicity, we'll assume it's supported on Windows
    return true;
#else
    // On Unix-like systems, check if we're outputting to a terminal
    return isatty(fileno(stdout));
#endif
}

} // namespace Logger
