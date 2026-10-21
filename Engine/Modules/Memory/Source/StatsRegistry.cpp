// The process's stats registry: an intrusive list of the records that live inside their
// owners (TrackedBytes, Arena, ObjectPool<T>, each thread's scratch arena), guarded by one
// mutex that registration, unregistration, snapshots and the image-pin query take. Owners write
// their counts without it.
//
// One registry per process: Memory is spliced into Engine, and an executable that links
// Engine links no second copy of the module (cmake/EngineModuleDuplicationCheck.cmake).

#include "StatsRegistry.h"

#include "Memory/ImagePins.h"
#include "Memory/StatsRecord.h"
#include "Memory/StatsSnapshot.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace GameEngine::Memory
{
namespace
{

struct StatsRegistry
{
    std::mutex Mutex;
    Detail::StatsRecord* Newest = nullptr;
};

StatsRegistry& Registry()
{
    // Immortal: an owner declared as a static registers during its image's static
    // initialization and unregisters during static destruction, in an order no image
    // controls, so the registry is created on first use and never destroyed (the OS
    // reclaims it at exit).
    static StatsRegistry* const s_Registry = new StatsRegistry();
    return *s_Registry;
}

} // namespace

namespace Detail
{

void RegisterStats(StatsRecord& record, std::string_view name, StatsKind kind, Residency residency)
{
#ifndef NDEBUG
    if (name.size() > StatsRecord::kMaxNameLength)
    {
        std::fprintf(stderr, "Memory stats name '%.*s' is %zu characters; stats names are at most %zu: shorten it.\n",
                     static_cast<int>(name.size()), name.data(), name.size(), StatsRecord::kMaxNameLength);
        assert(false && "stats name too long; shorten the name passed to TrackedBytes, Arena or ObjectPool (named above)");
    }
#endif
    const std::size_t length = std::min(name.size(), StatsRecord::kMaxNameLength);
    std::memcpy(record.Name, name.data(), length);
    record.Name[length] = '\0';
    record.Kind = kind;
    record.Residency = residency;

    StatsRegistry& registry = Registry();
    const std::lock_guard lock(registry.Mutex);
    record.Previous = nullptr;
    record.Next = registry.Newest;
    if (registry.Newest != nullptr)
        registry.Newest->Previous = &record;
    registry.Newest = &record;
}

void UnregisterStats(StatsRecord& record)
{
    StatsRegistry& registry = Registry();
    const std::lock_guard lock(registry.Mutex);
    if (record.Previous != nullptr)
        record.Previous->Next = record.Next;
    else
        registry.Newest = record.Next;
    if (record.Next != nullptr)
        record.Next->Previous = record.Previous;
    record.Previous = nullptr;
    record.Next = nullptr;
}

std::vector<std::string> DescribeArenasPinningImage(std::uint64_t base, std::uint64_t size)
{
    std::vector<std::string> names;
    if (size == 0)
        return names;
    StatsRegistry& registry = Registry();
    const std::lock_guard lock(registry.Mutex);
    for (const StatsRecord* record = registry.Newest; record != nullptr; record = record->Next)
    {
        // Acquire pairs with the owner's release stores: a non-zero Low comes with its High
        // (the first record), and a bound read as cleared comes after its destructors ran.
        const std::uint64_t low = record->DestructorLow.load(std::memory_order_acquire);
        if (low == 0)
            continue;
        const std::uint64_t high = record->DestructorHigh.load(std::memory_order_acquire);
        // [low, high] meets [base, base + size), written without forming base + size.
        if (high < base || (low >= base && low - base >= size))
            continue;
        const auto storage = reinterpret_cast<std::uintptr_t>(record);
        if (storage >= base && storage - base < size)
            continue;
        names.emplace_back(record->Name);
    }
    return names;
}

} // namespace Detail

std::vector<StatsEntry> SnapshotStats()
{
    std::vector<StatsEntry> entries;
    StatsRegistry& registry = Registry();
    const std::lock_guard lock(registry.Mutex);
    for (const Detail::StatsRecord* record = registry.Newest; record != nullptr; record = record->Next)
        entries.push_back({record->Name, record->Kind, record->Residency,
                           record->Bytes.load(std::memory_order_relaxed),
                           record->ReservedBytes.load(std::memory_order_relaxed),
                           record->HighWaterBytes.load(std::memory_order_relaxed),
                           record->Blocks.load(std::memory_order_relaxed), record->ThreadId,
                           record->OutOfOrder.load(std::memory_order_relaxed),
                           record->Shrinks.load(std::memory_order_relaxed)});
    return entries;
}

} // namespace GameEngine::Memory
