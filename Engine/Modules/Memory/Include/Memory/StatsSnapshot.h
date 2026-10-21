#pragma once

#include "Memory/Residency.h"
#include "Memory/StatsKind.h"

#include <cstdint>
#include <string>
#include <vector>

namespace GameEngine::Memory
{

/// One registered stats record, copied out of the registry.
struct StatsEntry
{
    std::string Name;
    StatsKind Kind = StatsKind::TrackedBytes;
    Memory::Residency Residency = Memory::Residency::Cpu;
    /// A `TrackedBytes`' count; an arena's bytes in use; a pool's live slot bytes; a range
    /// allocator's live range bytes (`StatsKind`).
    std::uint64_t Bytes = 0;
    /// An arena's or a pool's block bytes, its largest bytes in use so far (a pool's equals
    /// its `Bytes`, both as of its last block acquisition), and its block count; for a range
    /// allocator, the end of its highest range ever handed out, its most live bytes and its
    /// outstanding ranges; 0 for a `TrackedBytes`.
    std::uint64_t ReservedBytes = 0;
    std::uint64_t HighWaterBytes = 0;
    std::uint64_t Blocks = 0;
    /// A scratch entry's thread (the OS thread id: `GetCurrentThreadId` on Windows,
    /// `pthread_threadid_np` on macOS, `gettid` on Linux), its allocations through a scope
    /// that was not the innermost, and the times its retained block was released; 0 for
    /// every other kind. A scratch entry's arena counts are written when a block is acquired
    /// and at every 64th outermost close.
    std::uint64_t ThreadId = 0;
    std::uint64_t OutOfOrder = 0;
    std::uint64_t Shrinks = 0;
};

/// Copies every record registered at the moment of the call (every live `TrackedBytes`,
/// `Arena`, `ObjectPool` and `RangeAllocator`, and the scratch arena of every thread that has opened a
/// `ScratchScope` and not exited), newest first. Takes the registry mutex; meant for tools,
/// tests and debug views, not for a per-frame path.
std::vector<StatsEntry> SnapshotStats();

} // namespace GameEngine::Memory
