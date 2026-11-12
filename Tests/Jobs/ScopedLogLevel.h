#pragma once

#include "Logger/Logger.h"

namespace GameEngine::Tests
{

// Sets the global log level, and sets back the level that it found however the test leaves the scope.
// A gtest ASSERT_ returns from the test body, so a level that the body sets back only at its end stays for
// the tests that run after it. Declare it before the pool, so that the level also covers the pool's shutdown.
class ScopedLogLevel
{
  public:
    explicit ScopedLogLevel(Logger::LogLevel level) : m_Previous(Logger::Log::GetLogLevel())
    {
        Logger::Log::SetLogLevel(level);
    }

    ~ScopedLogLevel()
    {
        Logger::Log::SetLogLevel(m_Previous);
    }

    ScopedLogLevel(const ScopedLogLevel&) = delete;
    ScopedLogLevel& operator=(const ScopedLogLevel&) = delete;

  private:
    Logger::LogLevel m_Previous;
};

} // namespace GameEngine::Tests
