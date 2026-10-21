#pragma once

#include "Memory/Residency.h"
#include "Memory/StatsRecord.h"
#include "Types/FalseSharing.h"

#include <cstdint>
#include <string_view>

namespace GameEngine::Memory
{

/// A named byte count that the owning module keeps current and the stats registry
/// reports (`Memory::SnapshotStats`), for memory the module holds outside the general
/// heap's view: device buffers, pools, caches.
///
/// Declare it once per owner, usually as a static beside the code that holds the bytes,
/// and report changes as deltas:
///
///     Memory::TrackedBytes s_Resident{"Animation.RetargetGPUData", Memory::Residency::Gpu};
///     s_Resident.Add(grownBytes);
///     s_Resident.Subtract(releasedBytes);
///
/// The name is copied, so a temporary string is fine; names are at most 47 characters.
/// `Add` and `Subtract` are relaxed atomics, safe from any thread, with no lock and no
/// allocation. The object is registered for its whole life, so it cannot be copied or
/// moved.
class TrackedBytes
{
  public:
    TrackedBytes(std::string_view name, Residency residency);
    ~TrackedBytes();

    TrackedBytes(const TrackedBytes&) = delete;
    TrackedBytes& operator=(const TrackedBytes&) = delete;
    TrackedBytes(TrackedBytes&&) = delete;
    TrackedBytes& operator=(TrackedBytes&&) = delete;

    void Add(std::uint64_t bytes);
    /// Subtracts at most the current count: a release reported larger than what was
    /// added leaves 0, never a wrapped count.
    void Subtract(std::uint64_t bytes);

  private:
    FalseSharingPadded<Detail::StatsRecord> m_Record;
};

} // namespace GameEngine::Memory
