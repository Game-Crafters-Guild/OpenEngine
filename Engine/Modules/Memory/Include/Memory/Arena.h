#pragma once

#include "Memory/AddressSanitizer.h"
#include "Memory/SerialUseCheck.h"
#include "Memory/StatsRecord.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <memory_resource>
#include <new>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace GameEngine::Memory
{

namespace Detail
{
class ScratchThread;
/// Selects the constructor of a thread's scratch arena.
struct ScratchArenaTag
{
};
} // namespace Detail

/// The first block an `Arena` acquires, unless its constructor names another size.
inline constexpr std::size_t kArenaFirstBlockBytes = 64 * 1024;
/// The largest block one growth step acquires; a single larger request gets a block of its
/// own size, and the block `Reset` coalesces a chain into is not capped.
inline constexpr std::size_t kArenaMaxBlockBytes = 4 * 1024 * 1024;

/// A linear allocator: allocations bump a cursor through chained blocks, and memory comes
/// back all at once, at `Rewind` (to a marker) or `Reset` (everything). Nothing is freed one
/// allocation at a time.
///
///     Memory::Arena m_FrameArena{"MyGame.Pathfinding"};
///     PathNode* nodes = m_FrameArena.AllocateArray<PathNode>(count).data();
///     Request* request = m_FrameArena.New<Request>(goal);   // ~Request runs at Reset
///     std::pmr::vector<int> open{&m_FrameArena.Resource()};
///     ...
///     m_FrameArena.Reset();                                 // once per frame
///
/// - **Threading.** Externally serialized: one user at a time, on any thread, ordered by a
///   join, a lock or the frame. Debug and DebugFast assert two users at once. Any thread may
///   read memory it was handed.
/// - **Lifetime.** An allocation lives until the `Rewind` or `Reset` that covers it, or the
///   arena's destruction. Pointers stay stable until then: blocks are chained, never moved.
/// - **Alignment.** Any power of two, applied to the absolute address.
/// - **Growth.** A request that does not fit takes the next retained block that fits, else a
///   new block of twice the previous one (at most `kArenaMaxBlockBytes` per step, or the
///   request's size if larger). `Reset` replaces a chain of several blocks by one block of
///   the high-water size, so a steady frame bumps one block and allocates nothing.
/// - **Destructors.** `New<T>` records `~T` for a type that is not trivially destructible;
///   `Rewind` and `Reset` run the recorded destructors last-in first-out. A recorded
///   destructor whose code is in a game module keeps that module's code loaded at a hot
///   reload until the arena runs it (the editor log names the arena).
/// - **Failure.** `std::bad_alloc` when a block cannot be acquired; never null.
/// - **Stats.** Registered under `statsName` (copied, at most 47 characters) in
///   `Memory::SnapshotStats`: bytes in use, reserved, high-water and blocks, written when a
///   block is acquired and at `Rewind` and `Reset`, never per allocation.
///
/// Debug and DebugFast fill fresh allocations with 0xCD and rewound memory with 0xDD; under
/// AddressSanitizer, memory not handed out (block tails, rewound ranges) is poisoned.
class Arena
{
  public:
    /// A position to `Rewind` to: valid until a `Rewind` to an earlier position or a `Reset`.
    struct Marker
    {
        std::uint32_t Block = 0;
        std::uint32_t DestructorCount = 0;
        std::size_t Offset = 0;
    };

    /// Acquires nothing until the first allocation. `statsName` must not be empty.
    explicit Arena(std::string_view statsName, std::size_t firstBlockBytes = kArenaFirstBlockBytes);
    /// Runs the recorded destructors, then frees every block.
    ~Arena();

    // Resource() and the stats record point into the arena.
    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;
    Arena(Arena&&) = delete;
    Arena& operator=(Arena&&) = delete;

    /// `alignment` is a power of two. A zero-byte request returns a valid address that may
    /// equal the next allocation's.
    void* Allocate(std::size_t bytes, std::size_t alignment = alignof(std::max_align_t));

    /// Constructs a `T` in the arena. Its destructor is recorded, and run by `Rewind` or
    /// `Reset`, only when `T` is not trivially destructible. The constructor may allocate
    /// from this arena.
    template <class T, class... Args>
    T* New(Args&&... args);

    /// `count` value-initialised elements (zeroes for arithmetic types).
    template <class T>
    std::span<T> AllocateArray(std::size_t count);

    /// `count` elements left indeterminate, for arrays the caller fills before reading.
    template <class T>
    std::span<T> AllocateArrayForOverwrite(std::size_t count);

    /// A copy of `source` in the arena.
    template <class T>
    std::span<T> CopyArray(std::span<const T> source);

    Marker GetMarker() const;
    /// Runs the destructors recorded after `marker`, newest first, then moves the cursor back
    /// to it. Every block is kept.
    void Rewind(Marker marker);
    /// Runs every recorded destructor, newest first, and empties the arena. A chain of several
    /// blocks becomes one block of the high-water size.
    void Reset();

    /// A memory resource over this arena for `std::pmr` containers: allocation is
    /// `Allocate`, deallocation does nothing (the memory returns at `Rewind` or `Reset`).
    std::pmr::memory_resource& Resource() { return m_Resource; }

    /// Bytes handed out since the last `Reset`, alignment padding included.
    std::size_t BytesUsed() const;
    /// Bytes in the blocks the arena holds.
    std::size_t BytesReserved() const;

  private:
    /// A thread's `ScratchScope` state owns one arena through these: a scratch stats record,
    /// the peak its hysteresis reads at the outermost close, a rewind and a reset that leave the
    /// stats for later with inline fast paths for a steady scope's close, and the release of its
    /// block.
    friend class Detail::ScratchThread;

    /// Registers as `StatsKind::Scratch` with the thread's OS id.
    Arena(Detail::ScratchArenaTag, std::string_view statsName, std::uint64_t threadId);
    /// The most bytes in use since the last `Reset`, the bytes in use now included.
    std::size_t PeakBytesSinceReset() const;
    /// Frees every block of an empty arena; the next allocation acquires a block of
    /// `firstBlockBytes`, which also becomes the smallest block `Reset` coalesces into.
    void ReleaseBlocks(std::size_t firstBlockBytes);
    /// `Rewind`, with the stats stores only when `publishStats`: a nested scratch scope's close
    /// never publishes. The peak since the last `Reset` still takes what it rewinds.
    void RewindPublishing(Marker marker, bool publishStats);
    /// `Reset`, with the stats stores only when `publishStats`: a scratch arena publishes at
    /// every 64th outermost close.
    void ResetPublishing(bool publishStats);
    /// `ResetPublishing(false)` for an arena of one block holding no recorded destructor (a
    /// steady outermost scratch close), inline. On success `peakBytes` is what
    /// `PeakBytesSinceReset` read before the reset; any other arena returns false unchanged.
    bool TryResetOneBlock(std::size_t& peakBytes);
    /// `RewindPublishing(marker, false)` for a marker in the current block with no destructor
    /// recorded after it (a steady nested scratch close), inline; false, changing nothing,
    /// otherwise.
    bool TryRewindWithinCurrentBlock(Marker marker);

    class ArenaResource final : public std::pmr::memory_resource
    {
      public:
        explicit ArenaResource(Arena& arena) : m_Arena(arena) {}

      private:
        void* do_allocate(std::size_t bytes, std::size_t alignment) override;
        void do_deallocate(void*, std::size_t, std::size_t) override {}
        bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override { return this == &other; }

        Arena& m_Arena;
    };

    struct Block
    {
        std::byte* Data = nullptr;
        std::size_t Capacity = 0;
        /// The block's cursor offset; for the current block, as of the last block switch.
        std::size_t Used = 0;
    };

    struct DestructorRecord
    {
        void* Object = nullptr;
        void (*Destroy)(void*) = nullptr;
    };

    template <class T>
    static void Destroy(void* object)
    {
        static_cast<T*>(object)->~T();
    }

    /// The fast path's bump into the current block; null when the request does not fit.
    void* TryBump(std::size_t bytes, std::size_t alignment);
    /// Marks `[begin, begin + bytes)` as rewound: filled with 0xDD where instrumentation is on,
    /// and poisoned under AddressSanitizer, so a read through a pointer that outlived its Rewind
    /// or Reset shows. The range holds alignment padding that was never handed out and is still
    /// poisoned, so it is unpoisoned before the fill.
    static void RetireRange(std::byte* begin, std::size_t bytes);
    /// Serves a request the current block cannot: the next retained block that fits, else a
    /// new one.
    void* AllocateFromNextBlock(std::size_t bytes, std::size_t alignment);
    void* AllocateArrayBytes(std::size_t count, std::size_t elementBytes, std::size_t alignment);
    /// Makes room for one more destructor record before the object is constructed.
    void ReserveDestructorRecord();
    void RecordDestructor(void* object, void (*destroy)(void*));
    /// Pops and runs the recorded destructors above `count`, newest first, each popped before
    /// it runs, so one that records a destructor has that record run by the same walk.
    void RunDestructorsAbove(std::uint32_t count);
    void ClearDestructorBoundsIfNoneRecorded();
    void PublishStats();

    /// Holds the serial-use word for one state change (Debug and DebugFast; empty otherwise),
    /// released on every exit, a throw included.
    class UseScope
    {
      public:
        explicit UseScope([[maybe_unused]] Arena& arena)
#if GE_DEBUG_INSTRUMENTATION
            : m_Arena(arena)
        {
            m_Arena.m_SerialUse.Enter("Arena");
        }
        ~UseScope() { m_Arena.m_SerialUse.Exit(); }
#else
        {
        }
#endif
        UseScope(const UseScope&) = delete;
        UseScope& operator=(const UseScope&) = delete;

      private:
#if GE_DEBUG_INSTRUMENTATION
        Arena& m_Arena;
#endif
    };

    std::vector<Block> m_Blocks;
    std::vector<DestructorRecord> m_Destructors;
    /// The bump cursor and the end of the current block. With no block both are such that
    /// every request misses: 1 and 0. Never adjacent: the compiler reads adjacent members with
    /// one paired load, which cannot be forwarded from the previous bump's 8-byte store of the
    /// cursor and stalls every bump of a sequence until that store retires.
    std::uintptr_t m_Limit = 0;
    std::size_t m_Current = 0;
    std::uintptr_t m_Cursor = 1;
    /// Bytes used in the blocks before the current one.
    std::size_t m_UsedBeforeCurrent = 0;
    /// The largest `BytesUsed()` since the last `Reset`: the size `Reset` coalesces into.
    std::size_t m_PeakSinceReset = 0;
    std::size_t m_FirstBlockBytes;
    ArenaResource m_Resource{*this};
    /// Not false-sharing aligned: written only at block acquisition, `Rewind` and `Reset`, so
    /// an owner that holds an `Arena` by value is not over-aligned.
    Detail::StatsRecord m_Record;
#if GE_DEBUG_INSTRUMENTATION
    Detail::SerialUseCheck m_SerialUse;
#endif
};

static_assert(alignof(Arena) <= alignof(std::max_align_t),
              "Arena must not be over-aligned: a class holding one by value would be padded (MSVC C4324)");

inline void* Arena::TryBump(std::size_t bytes, std::size_t alignment)
{
    const std::uintptr_t aligned = (m_Cursor + (alignment - 1)) & ~static_cast<std::uintptr_t>(alignment - 1);
    if (aligned > m_Limit || bytes > m_Limit - aligned)
        return nullptr;
    m_Cursor = aligned + bytes;
    void* memory = reinterpret_cast<void*>(aligned);
#if defined(GE_MEMORY_ASAN)
    ASAN_UNPOISON_MEMORY_REGION(memory, bytes);
#endif
#if GE_DEBUG_INSTRUMENTATION
    std::memset(memory, 0xCD, bytes);
#endif
    return memory;
}

inline Arena::Marker Arena::GetMarker() const
{
    Marker marker;
    marker.Block = static_cast<std::uint32_t>(m_Current);
    marker.DestructorCount = static_cast<std::uint32_t>(m_Destructors.size());
    marker.Offset = m_Blocks.empty() ? 0 : m_Cursor - reinterpret_cast<std::uintptr_t>(m_Blocks[m_Current].Data);
    return marker;
}

inline void Arena::RetireRange([[maybe_unused]] std::byte* begin, [[maybe_unused]] std::size_t bytes)
{
#if defined(GE_MEMORY_ASAN)
    ASAN_UNPOISON_MEMORY_REGION(begin, bytes);
#endif
#if GE_DEBUG_INSTRUMENTATION
    std::memset(begin, 0xDD, bytes);
#endif
#if defined(GE_MEMORY_ASAN)
    ASAN_POISON_MEMORY_REGION(begin, bytes);
#endif
}

inline bool Arena::TryResetOneBlock(std::size_t& peakBytes)
{
    if (m_Blocks.size() != 1 || !m_Destructors.empty())
        return false;
    const UseScope use(*this);
    Block& block = m_Blocks.front();
    const std::uintptr_t data = reinterpret_cast<std::uintptr_t>(block.Data);
    const std::size_t used = m_Cursor - data;
    peakBytes = std::max(m_PeakSinceReset, used);
    RetireRange(block.Data, used);
    block.Used = 0;
    m_Cursor = data;
    m_PeakSinceReset = 0;
    return true;
}

inline bool Arena::TryRewindWithinCurrentBlock(Marker marker)
{
    if (m_Blocks.empty() || marker.Block != m_Current || marker.DestructorCount != m_Destructors.size())
        return false;
    const UseScope use(*this);
    const std::uintptr_t data = reinterpret_cast<std::uintptr_t>(m_Blocks[m_Current].Data);
    const std::uintptr_t target = data + marker.Offset;
    assert(target <= m_Cursor && "Rewind to a marker taken before a later Rewind or Reset");
    m_PeakSinceReset = std::max(m_PeakSinceReset, m_UsedBeforeCurrent + (m_Cursor - data));
    RetireRange(reinterpret_cast<std::byte*>(target), m_Cursor - target);
    m_Cursor = target;
    return true;
}

inline void* Arena::Allocate(std::size_t bytes, std::size_t alignment)
{
    assert(alignment != 0 && (alignment & (alignment - 1)) == 0 && "Arena alignment must be a power of two");
    const UseScope use(*this);
    void* memory = TryBump(bytes, alignment);
    if (memory == nullptr)
        memory = AllocateFromNextBlock(bytes, alignment);
    return memory;
}

template <class T, class... Args>
T* Arena::New(Args&&... args)
{
    void* memory = Allocate(sizeof(T), alignof(T));
    if constexpr (std::is_trivially_destructible_v<T>)
    {
        return ::new (memory) T(std::forward<Args>(args)...);
    }
    else
    {
        ReserveDestructorRecord();
        T* object = ::new (memory) T(std::forward<Args>(args)...);
        RecordDestructor(object, &Destroy<T>);
        return object;
    }
}

template <class T>
std::span<T> Arena::AllocateArray(std::size_t count)
{
    static_assert(std::is_trivially_destructible_v<T>,
                  "Arena arrays hold trivially destructible types; New<T>() records a destructor per object");
    T* elements = static_cast<T*>(AllocateArrayBytes(count, sizeof(T), alignof(T)));
    std::uninitialized_value_construct_n(elements, count);
    return {elements, count};
}

template <class T>
std::span<T> Arena::AllocateArrayForOverwrite(std::size_t count)
{
    static_assert(std::is_trivially_destructible_v<T>,
                  "Arena arrays hold trivially destructible types; New<T>() records a destructor per object");
    T* elements = static_cast<T*>(AllocateArrayBytes(count, sizeof(T), alignof(T)));
    std::uninitialized_default_construct_n(elements, count);
    return {elements, count};
}

template <class T>
std::span<T> Arena::CopyArray(std::span<const T> source)
{
    static_assert(std::is_trivially_destructible_v<T>,
                  "Arena arrays hold trivially destructible types; New<T>() records a destructor per object");
    T* elements = static_cast<T*>(AllocateArrayBytes(source.size(), sizeof(T), alignof(T)));
    std::uninitialized_copy(source.begin(), source.end(), elements);
    return {elements, source.size()};
}

} // namespace GameEngine::Memory
