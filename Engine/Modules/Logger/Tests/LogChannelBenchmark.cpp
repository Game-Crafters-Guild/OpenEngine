// Advisory producer-cost benchmark, not a timing-based unit gate. Prints the cost of a filtered call through
// GE_LOG, LOG_INFO and Log::Info, and of an enabled record through GE_LOG and Log::Warning.
// Only a Release build gives a number worth quoting.
#include "Logger/CallbackSink.h"
#include "Logger/LogChannel.h"
#include "Logger/Logger.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>

namespace
{
GE_LOG_CHANNEL(BenchmarkLog, "Benchmark", Warning)

using Clock = std::chrono::steady_clock;
constexpr int kFilteredCalls = 50'000'000;
constexpr int kEnabledCalls = 200'000;

double NanosecondsPerCall(Clock::time_point begin, int calls)
{
    return std::chrono::duration<double, std::nano>(Clock::now() - begin).count() / calls;
}
} // namespace

int main()
{
    using namespace Logger;
    Log::Config config;
    config.GlobalMinLevel = LogLevel::Warning;
    Log::Initialize(config);
    Log::ClearSinks();
    std::atomic<int> delivered{0};
    auto sink = std::make_unique<CallbackSink>();
    sink->RegisterCallback([&delivered](const LogMessage&) { ++delivered; });
    Log::AddSink(std::move(sink));

    auto begin = Clock::now();
    for (int i = 0; i < kFilteredCalls; ++i)
        GE_LOG(BenchmarkLog, Info, "filtered {}", i);
    const double filteredChannel = NanosecondsPerCall(begin, kFilteredCalls);

    begin = Clock::now();
    for (int i = 0; i < kFilteredCalls; ++i)
        Log::Info("filtered {}", i);
    const double filteredCore = NanosecondsPerCall(begin, kFilteredCalls);

    begin = Clock::now();
    for (int i = 0; i < kFilteredCalls; ++i)
        LOG_INFO("filtered {}", i);
    const double filteredMacro = NanosecondsPerCall(begin, kFilteredCalls);

    begin = Clock::now();
    for (int i = 0; i < kEnabledCalls; ++i)
        GE_LOG(BenchmarkLog, Warning, "enabled {}", i);
    const double enabledChannel = NanosecondsPerCall(begin, kEnabledCalls);
    Log::Flush();

    begin = Clock::now();
    for (int i = 0; i < kEnabledCalls; ++i)
        Log::Warning("enabled {}", i);
    const double enabledCore = NanosecondsPerCall(begin, kEnabledCalls);
    Log::Flush();

    const int records = delivered.load();
    std::printf("filtered GE_LOG: %.2f ns/call\nfiltered LOG_INFO: %.2f ns/call\nfiltered Log::Info: %.2f ns/call\n"
                "enabled GE_LOG: %.1f ns/call\nenabled Log::Warning: %.1f ns/call (%d of %d records delivered)\n",
                filteredChannel, filteredMacro, filteredCore, enabledChannel, enabledCore, records,
                2 * kEnabledCalls);
    Log::Shutdown();
    return records == 2 * kEnabledCalls ? 0 : 1;
}
