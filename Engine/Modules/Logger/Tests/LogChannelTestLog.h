#pragma once

#include "Logger/LogChannel.h"

namespace LoggerTests
{
GE_LOG_CHANNEL(QuietTestLog, "Tests.Quiet", Warning)

/// The QuietTestLog accessor as LogChannelSecondUnit.cpp sees it.
const Logger::LogChannel& QuietTestLogInSecondUnit();

/// Logs one Debug record on QuietTestLog from LogChannelSecondUnit.cpp.
void LogDebugInSecondUnit(int value);
} // namespace LoggerTests
