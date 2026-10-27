#include "LoggerModuleProbe.h"
#include "Logger/Logger.h"

namespace
{
LoggerModuleProbeStats* s_Stats = nullptr;
bool s_LogOnDetach = false;

struct Argument
{
    unsigned Value;
    explicit Argument(unsigned value) : Value(value) { ++s_Stats->LiveArguments; }
    Argument(const Argument& value) : Argument(value.Value) {}
    Argument(Argument&& value) noexcept : Argument(value.Value) {}
    ~Argument() { --s_Stats->LiveArguments; }
};
struct DetachLog
{
    ~DetachLog()
    {
        if (s_LogOnDetach)
            Logger::Log::Info("detach log {}", 71);
    }
} s_Detach;
} // namespace

template <> struct std::formatter<Argument> : std::formatter<unsigned>
{
    auto format(const Argument& argument, std::format_context& context) const
    {
        ++s_Stats->Formatted;
        return std::formatter<unsigned>::format(argument.Value, context);
    }
};

extern "C" __declspec(dllexport) void ConfigureProbe(LoggerModuleProbeStats* stats,
                                                     bool logOnDetach)
{
    s_Stats = stats;
    s_LogOnDetach = logOnDetach;
}

extern "C" __declspec(dllexport) void EmitProbe(unsigned value, bool formatted)
{
    // The file, the function and the channel name are this image's literals; the queued record must own copies.
    if (formatted)
        GE_LOG(ProbeLog, Info, "probe value {}", Argument(value));
    else
        GE_LOG(ProbeLog, Info, "probe plain text");
}
