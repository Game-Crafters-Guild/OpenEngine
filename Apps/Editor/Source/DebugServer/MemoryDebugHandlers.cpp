#include "DebugServer/MemoryDebugHandlers.h"

#include "Core/Application.h"
#include "DebugServer/EditorDebugServer.h"
#include "Memory/StatsSnapshot.h"
#include "Platform/Thread.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#elif defined(__linux__)
#include <cstdio>
#include <unistd.h>
#endif

namespace GameEngine
{

using json = nlohmann::json;

namespace
{

const char* KindName(Memory::StatsKind kind)
{
    switch (kind)
    {
    case Memory::StatsKind::TrackedBytes:
        return "TrackedBytes";
    case Memory::StatsKind::Arena:
        return "Arena";
    case Memory::StatsKind::Scratch:
        return "Scratch";
    case Memory::StatsKind::ObjectPool:
        return "ObjectPool";
    case Memory::StatsKind::RangeAllocator:
        return "RangeAllocator";
    }
    return "Unknown";
}

// The bytes of this process resident in physical memory now (not the peak).
std::optional<std::uint64_t> ProcessResidentBytes()
{
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
        return static_cast<std::uint64_t>(counters.WorkingSetSize);
#elif defined(__APPLE__)
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) ==
        KERN_SUCCESS)
        return static_cast<std::uint64_t>(info.resident_size);
#elif defined(__linux__)
    if (std::FILE* file = std::fopen("/proc/self/statm", "r"))
    {
        unsigned long long totalPages = 0;
        unsigned long long residentPages = 0;
        const int read = std::fscanf(file, "%llu %llu", &totalPages, &residentPages);
        std::fclose(file);
        if (read == 2)
            return static_cast<std::uint64_t>(residentPages) * static_cast<std::uint64_t>(sysconf(_SC_PAGESIZE));
    }
#endif
    return std::nullopt;
}

json HandleMemoryStats(const Application& app)
{
    const std::vector<Memory::StatsEntry> entries = Memory::SnapshotStats();
    json list = json::array();
    std::uint64_t scratchThreads = 0;
    std::uint64_t scratchBytes = 0;
    std::uint64_t scratchReserved = 0;
    std::uint64_t scratchOutOfOrder = 0;
    std::uint64_t scratchShrinks = 0;
    for (const Memory::StatsEntry& entry : entries)
    {
        json item = {
            {"name", entry.Name},
            {"kind", KindName(entry.Kind)},
            {"residency", entry.Residency == Memory::Residency::Gpu ? "Gpu" : "Cpu"},
            {"bytes", entry.Bytes},
        };
        if (entry.Kind != Memory::StatsKind::TrackedBytes)
        {
            item["reservedBytes"] = entry.ReservedBytes;
            item["highWaterBytes"] = entry.HighWaterBytes;
            item["blocks"] = entry.Blocks;
        }
        if (entry.Kind == Memory::StatsKind::Scratch)
        {
            item["threadId"] = entry.ThreadId;
            item["threadName"] = Platform::GetThreadName(entry.ThreadId);
            item["outOfOrder"] = entry.OutOfOrder;
            item["shrinks"] = entry.Shrinks;
            ++scratchThreads;
            scratchBytes += entry.Bytes;
            scratchReserved += entry.ReservedBytes;
            scratchOutOfOrder += entry.OutOfOrder;
            scratchShrinks += entry.Shrinks;
        }
        list.push_back(std::move(item));
    }

    json reply = {
        {"frame", app.GetFrameCount()},
        {"entries", std::move(list)},
        {"scratch",
         {{"threads", scratchThreads},
          {"bytes", scratchBytes},
          {"reservedBytes", scratchReserved},
          {"outOfOrder", scratchOutOfOrder},
          {"shrinks", scratchShrinks}}},
    };
    if (const std::optional<std::uint64_t> resident = ProcessResidentBytes())
        reply["processResidentBytes"] = *resident;
    return reply;
}

} // namespace

void RegisterMemoryDebugHandlers(EditorDebugServer& server, const Application& app)
{
    // get_memory_stats — the Memory module's stats registry right now (Memory::SnapshotStats).
    // No parameters. Reply:
    //   frame: the editor's frame count when read.
    //   entries[]: {name, kind, residency, bytes} per registered record, newest first; kind is
    //     "TrackedBytes" (bytes: what its owner reported), "Arena", "Scratch" or "ObjectPool"
    //     (bytes in use, plus reservedBytes, highWaterBytes, blocks; an "ObjectPool" counts slot
    //     bytes as of its last block acquisition); a "Scratch" entry is one thread's
    //     ScratchScope arena and adds threadId (the OS id), threadName (empty when the thread
    //     has none), outOfOrder (allocations through a scope that was not the innermost,
    //     served from the heap) and shrinks (retained blocks released by the hysteresis).
    //   scratch: {threads, bytes, reservedBytes, outOfOrder, shrinks} summed over the scratch
    //     entries. A thread writes its scratch counts when it acquires a block and at every
    //     64th outermost close, so bytes lags what is open now.
    //   processResidentBytes: the process's resident size now, where the platform reports it.
    server.RegisterHandler("get_memory_stats",
                           [&app](const EditorDebugServer::RequestContext&) -> json { return HandleMemoryStats(app); });
}

} // namespace GameEngine
