#pragma once

#include <cstdint>

namespace GameEngine::Memory
{

/// What registered a stats record, and so which of its counts are meaningful.
enum class StatsKind : std::uint8_t
{
    /// A `TrackedBytes`: `Bytes` is what its owner reported; the arena counts are 0.
    TrackedBytes,
    /// An `Arena`: `Bytes` is the bytes in use, beside the reserved bytes, the high-water
    /// mark and the block count.
    Arena,
    /// One thread's `ScratchScope` arena: the arena counts, plus the thread's OS id, its
    /// out-of-order allocations and its shrinks.
    Scratch,
    /// An `ObjectPool<T>`, in slot bytes: `Bytes` the live objects' slots and `HighWaterBytes`
    /// the most slots ever live, both as of the last block acquisition (the pool acquires a
    /// block only when every slot is live, so the two are equal); `ReservedBytes` its block
    /// bytes and `Blocks` its block count. `ObjectPool::LiveCount()` is exact at any time.
    ObjectPool,
    /// A `RangeAllocator`, in bytes (its units times its unit size): `Bytes` the live ranges
    /// (those waiting for `ReclaimThrough` included), `HighWaterBytes` the most ever live,
    /// `ReservedBytes` the end of the highest range ever handed out (the span of the owner's
    /// storage it has used) and `Blocks` the ranges handed out and not returned.
    RangeAllocator,
};

} // namespace GameEngine::Memory
