#pragma once
#include "Logger/LogChannel.h"

#include <atomic>

GE_LOG_CHANNEL(ProbeLog, "Tests.Probe", Info)

struct LoggerModuleProbeStats
{
    std::atomic<unsigned> Formatted{0};
    std::atomic<unsigned> LiveArguments{0};
};

using ConfigureLoggerModuleProbe = void (*)(LoggerModuleProbeStats*, bool);
using EmitLoggerModuleProbe = void (*)(unsigned, bool);
