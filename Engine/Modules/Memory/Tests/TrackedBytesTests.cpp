// TrackedBytes and the stats registry that reports it: what a snapshot sees, when, and
// under concurrent registration.

#include "Memory/StatsSnapshot.h"
#include "Memory/TrackedBytes.h"
#include "Types/FalseSharing.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{
using GameEngine::kFalseSharingSeparation;
using GameEngine::Memory::Residency;
using GameEngine::Memory::SnapshotStats;
using GameEngine::Memory::StatsEntry;
using GameEngine::Memory::TrackedBytes;

static_assert(alignof(TrackedBytes) == kFalseSharingSeparation,
              "each record sits on its own false-sharing span");
static_assert(sizeof(TrackedBytes) % kFalseSharingSeparation == 0,
              "a TrackedBytes array never puts two records on one span");

std::optional<StatsEntry> FindEntry(std::string_view name)
{
    for (StatsEntry& entry : SnapshotStats())
    {
        if (entry.Name == name)
            return std::move(entry);
    }
    return std::nullopt;
}

constexpr int kRegistrationThreads = 8;
constexpr int kRecordsPerThread = 2000;
constexpr std::string_view kWorkerPrefix = "Tests.Registry.Worker";

/// Registers and unregisters one record after another under a name of its own, then reports
/// that it is done.
void RegisterAndUnregisterRepeatedly(int thread, std::atomic<int>& runningWorkers)
{
    const std::string name = std::string(kWorkerPrefix) + std::to_string(thread);
    for (int record = 0; record < kRecordsPerThread; ++record)
    {
        TrackedBytes tracked(name, Residency::Cpu);
        tracked.Add(static_cast<std::uint64_t>(record));
        tracked.Subtract(static_cast<std::uint64_t>(record));
    }
    runningWorkers.fetch_sub(1, std::memory_order_release);
}

std::size_t CountEntriesStartingWith(const std::vector<StatsEntry>& entries, std::string_view prefix)
{
    std::size_t count = 0;
    for (const StatsEntry& entry : entries)
    {
        if (std::string_view(entry.Name).starts_with(prefix))
            ++count;
    }
    return count;
}
} // namespace

TEST(TrackedBytesTests, ASnapshotReportsTheNameResidencyAndBytes)
{
    TrackedBytes tracked("Tests.TrackedBytes.Reported", Residency::Gpu);
    tracked.Add(4096);
    tracked.Add(1024);
    tracked.Subtract(512);

    const std::optional<StatsEntry> entry = FindEntry("Tests.TrackedBytes.Reported");
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->Residency, Residency::Gpu);
    EXPECT_EQ(entry->Bytes, 4608u);
}

TEST(TrackedBytesTests, SubtractingMoreThanWasAddedLeavesZero)
{
    TrackedBytes tracked("Tests.TrackedBytes.Saturating", Residency::Cpu);
    tracked.Add(10);
    tracked.Subtract(25);
    ASSERT_TRUE(FindEntry("Tests.TrackedBytes.Saturating").has_value());
    EXPECT_EQ(FindEntry("Tests.TrackedBytes.Saturating")->Bytes, 0u) << "the count wrapped below zero";

    tracked.Add(5);
    EXPECT_EQ(FindEntry("Tests.TrackedBytes.Saturating")->Bytes, 5u);
}

TEST(StatsRegistryTests, ADestroyedTrackedBytesLeavesTheSnapshot)
{
    {
        TrackedBytes tracked("Tests.Registry.Destroyed", Residency::Cpu);
        ASSERT_TRUE(FindEntry("Tests.Registry.Destroyed").has_value());
    }
    EXPECT_FALSE(FindEntry("Tests.Registry.Destroyed").has_value());
}

TEST(StatsRegistryTests, ANameFromATemporaryStringSurvivesInTheSnapshot)
{
    // Longer than any standard library's small-string buffer, so the temporary's characters
    // are on the heap and freed with it; a record that kept a view of them reads freed memory,
    // which ASan reports and which otherwise reads back as the overwritten 'x's.
    const std::string expected = "Tests.Registry.NameFromATemporaryString.Heap";
    ASSERT_GT(expected.size(), sizeof(std::string));

    std::optional<TrackedBytes> tracked;
    {
        std::string temporary = expected;
        tracked.emplace(temporary, Residency::Cpu);
        temporary.assign(temporary.size(), 'x');
    }

    EXPECT_TRUE(FindEntry(expected).has_value()) << "the record lost the name it was constructed with";
}

TEST(StatsRegistryTests, ConcurrentRegistrationNeverHidesALiveRecord)
{
    TrackedBytes permanent("Tests.Registry.Permanent", Residency::Cpu);
    permanent.Add(42);

    std::atomic<int> runningWorkers{kRegistrationThreads};
    std::vector<std::thread> workers;
    workers.reserve(kRegistrationThreads);
    for (int thread = 0; thread < kRegistrationThreads; ++thread)
        workers.emplace_back(RegisterAndUnregisterRepeatedly, thread, std::ref(runningWorkers));

    int snapshots = 0;
    int snapshotsMissingThePermanentRecord = 0;
    std::size_t mostLiveWorkerRecords = 0;
    while (runningWorkers.load(std::memory_order_acquire) != 0)
    {
        const std::vector<StatsEntry> entries = SnapshotStats();
        ++snapshots;
        bool sawPermanent = false;
        for (const StatsEntry& entry : entries)
        {
            if (entry.Name == "Tests.Registry.Permanent")
                sawPermanent = entry.Bytes == 42;
        }
        if (!sawPermanent)
            ++snapshotsMissingThePermanentRecord;
        mostLiveWorkerRecords = std::max(mostLiveWorkerRecords, CountEntriesStartingWith(entries, kWorkerPrefix));
    }
    for (std::thread& worker : workers)
        worker.join();

    EXPECT_GT(snapshots, 0);
    EXPECT_EQ(snapshotsMissingThePermanentRecord, 0)
        << "a snapshot taken while other records registered and unregistered lost a live record";
    EXPECT_LE(mostLiveWorkerRecords, static_cast<std::size_t>(kRegistrationThreads))
        << "a snapshot listed a worker record after its TrackedBytes was destroyed";
    EXPECT_EQ(CountEntriesStartingWith(SnapshotStats(), kWorkerPrefix), 0u);
}
