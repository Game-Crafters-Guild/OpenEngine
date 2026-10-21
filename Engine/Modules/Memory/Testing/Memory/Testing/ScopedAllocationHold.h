#pragma once

#include "Memory/AllocationCounter.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace GameEngine::Memory::Testing
{

/// Parks the calling thread inside one of its own allocations, to test what other threads
/// see while an allocator is in the middle of acquiring memory. The `holdOnMatch`-th
/// allocation on this thread whose size equals `matchBytes` (0 matches every size) waits
/// inside the allocation hook, before it is served, until another thread calls `Release()`.
///
/// Construct it on the thread that will park, where the other thread can reach it (the
/// test's frame, or a pointer published to the other thread); call `WaitUntilHeld()` and
/// `Release()` from the other thread. They synchronize through an atomic inside the object.
///
///     // on the allocating thread
///     Memory::Testing::ScopedAllocationHold hold{kBlockBytes, 1};
///     published.store(&hold);
///     arena.Allocate(kBlockBytes);              // parks inside the block acquisition
///     // on the test thread
///     published.load()->WaitUntilHeld();
///     ...                                       // the allocator is now mid-acquisition
///     published.load()->Release();
///
/// It reaches every image the allocation hook reaches, as `ScopedAllocationFault` does.
/// One armed hold per thread (asserted in Debug); a parked thread spins, yielding, so it
/// allocates nothing while it waits.
class ScopedAllocationHold
{
  public:
    /// `holdOnMatch` counts from 1.
    ScopedAllocationHold(std::size_t matchBytes, std::uint32_t holdOnMatch);
    ~ScopedAllocationHold();

    ScopedAllocationHold(const ScopedAllocationHold&) = delete;
    ScopedAllocationHold& operator=(const ScopedAllocationHold&) = delete;
    ScopedAllocationHold(ScopedAllocationHold&&) = delete;
    ScopedAllocationHold& operator=(ScopedAllocationHold&&) = delete;

    /// Returns once the matching allocation is parked, or has been released.
    void WaitUntilHeld() const;

    /// Lets the parked allocation continue; one that has not arrived yet passes without
    /// parking.
    void Release();

  private:
    friend bool Detail::OnAllocation(std::size_t bytes);

    enum class State : std::uint8_t
    {
        Armed,
        Held,
        Released,
    };

    /// Called by the hook for each allocation on the owning thread: parks the matching one
    /// until `Release()`, once. Every match before the holdOnMatch-th passes.
    void ParkIfMatch(std::size_t bytes);

    std::size_t m_MatchBytes;
    /// Matches still to pass before the one that parks; read and written by the owning thread.
    std::uint32_t m_MatchesBeforeHold;
    std::atomic<State> m_State{State::Armed};
};

} // namespace GameEngine::Memory::Testing
