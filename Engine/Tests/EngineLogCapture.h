#pragma once

// Capture of the engine's log stream, for tests that assert on what a code path
// reports through Engine.dll.
//
// Engine is a SHARED library and Logger a STATIC one: engine code logs through
// Engine.dll's Logger state, not this executable's copy. The capture adopts the
// engine's state (RedirectToSharedState) or its sink sees nothing. Everything else
// is LogCapture.h's ScopedLogCapture: read its notes on proving a capture live and on
// restoring the logger.

#include "Core/EngineLoggerBridge.h"
#include "LogCapture.h"

#include <string>
#include <vector>

namespace GameEngine::TestLog
{

// Routes every engine log line at or above minLevel into *out for the scope's
// lifetime. Call Logger::Log::Flush() before reading: dispatch is asynchronous.
class ScopedEngineLogCapture
{
  public:
    explicit ScopedEngineLogCapture(std::vector<std::string>* out,
                                    Logger::LogLevel minLevel = Logger::LogLevel::Info)
        : m_Capture(out, minLevel)
    {
    }

  private:
    // Adopts the engine's Logger state when constructed; declared before m_Capture, so the
    // capture's level and sink are set on the engine's state.
    struct EngineLoggerStateAdoption
    {
        EngineLoggerStateAdoption() { Logger::Log::RedirectToSharedState(GameEngine::GetEngineLoggerState()); }
    };

    EngineLoggerStateAdoption m_Adoption;
    ScopedLogCapture m_Capture;
};

} // namespace GameEngine::TestLog
