// A second translation unit that includes the header with the QuietTestLog declaration.

#include "LogChannelTestLog.h"

namespace LoggerTests
{
const Logger::LogChannel& QuietTestLogInSecondUnit()
{
    return QuietTestLog();
}

void LogDebugInSecondUnit(int value)
{
    GE_LOG(QuietTestLog, Debug, "second unit {}", value);
}
} // namespace LoggerTests
