#pragma once

#include "Memory/AddressSanitizer.h"
#include "Memory/SerialUseCheck.h"
#include "Memory/StatsRecord.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <utility>
#include <vector>

namespace GameEngine::Memory
{
template <class T>
class ObjectPool;

namespace Detail
{

/// The untyped storage behind `ObjectPool<T>`: fixed-size slots in blocks that never move,
/// an intrusive last-in first-out freelist through the free slots, one bit per slot at the
/// end of each block, and the pool's stats record. `New` and `Delete` reach it through the
/// inline `Take` and `Return`; everything that runs once per block, or only in Debug, or only
/// while the pool is destroyed is out of line in `ObjectPool.cpp`, and none of it is a
/// template. Only `ObjectPool<T>` reaches its interface.
///
/// A slot's bit means "live" in every configuration. Debug and DebugFast write it at every
/// `Take` and `Return`, for the double-delete and foreign-pointer asserts; Release writes it
/// only when the pool is destroyed, deriving it from the blocks' used marks and the freelist.
class ObjectPoolStorage
{
    template <class>
    friend class Memory::ObjectPool;

    ObjectPoolStorage(std::string_view statsName, std::size_t objectBytes, std::size_t objectAlignment);
    /// Frees every block. The typed pool has destroyed the live objects first.
    ~ObjectPoolStorage();

    ObjectPoolStorage(const ObjectPoolStorage&) = delete;
    ObjectPoolStorage& operator=(const ObjectPoolStorage&) = delete;
    ObjectPoolStorage(ObjectPoolStorage&&) = delete;
    ObjectPoolStorage& operator=(ObjectPoolStorage&&) = delete;

    /// A slot for a new object: the freelist's head, else the next never-used slot of the
    /// newest block, else the first slot of a new block. Never null; `std::bad_alloc` when a
    /// block cannot be acquired.
    void* Take();
    /// Debug and DebugFast: asserts that `slot` holds a live object of this pool and marks it
    /// free, before its destructor runs. Nothing in Release.
    void CheckLiveAndMarkFree(void* slot);
    /// Puts the slot of an object whose destructor has run at the freelist's head.
    void Return(void* slot);

    /// True from the start of `DestroyLiveObjects` to the end of the storage's life.
    bool IsDestroying() const { return m_Destroying; }
    /// While destroying: true when `slot` still holds a live object, which is then marked
    /// destroyed; false for an object the destruction walk or an earlier `Delete` already
    /// destroyed.
    bool ClaimWhileDestroying(void* slot);
    /// While destroying: fills and poisons the slot of an object whose destructor has run.
    void RetireWhileDestroying(void* slot);
    /// Derives the live set and runs `destroy` on each live object once, walking the blocks
    /// in address order; each object is marked destroyed before its destructor runs.
    void DestroyLiveObjects(void (*destroy)(void*)) noexcept;

    std::size_t LiveCount() const { return m_LiveCount; }

    struct BlockDescriptor
    {
        std::byte* Begin = nullptr;
        std::uint32_t SlotCount = 0;
        /// One bit per slot, set when the slot is live; after the slots, inside the block.
        std::uint64_t* Bitmap = nullptr;
    };

    struct SlotLocation
    {
        std::size_t Block = 0;
        std::size_t Slot = 0;
    };

    /// Acquires a block and returns its first slot. Runs under the caller's serial-use hold.
    void* TakeFromNewBlock();
    /// Slots of `block` handed out at least once: all of them, except in the newest block.
    std::size_t UsedSlots(const BlockDescriptor& block) const;
    /// The block and slot index of `slot`, found by binary search over the descriptors;
    /// asserts (Debug and DebugFast) that it is a slot boundary in one of them.
    SlotLocation Locate(const void* slot) const;
    void MarkLive(void* slot);
    /// Debug and DebugFast, before the live set is derived: the per-operation bits agree with
    /// the freelist and the live count.
    void CheckBitsAgreeWithFreelist() const;
    void DeriveLiveSet();
    void PublishStats(std::size_t liveCount);

    /// Holds the serial-use word for one state change (Debug and DebugFast; empty otherwise).
    class UseScope
    {
      public:
        explicit UseScope([[maybe_unused]] ObjectPoolStorage& storage)
#if GE_DEBUG_INSTRUMENTATION
            : m_Storage(storage)
        {
            m_Storage.m_SerialUse.Enter("ObjectPool");
        }
        ~UseScope() { m_Storage.m_SerialUse.Exit(); }
#else
        {
        }
#endif
        UseScope(const UseScope&) = delete;
        UseScope& operator=(const UseScope&) = delete;

      private:
#if GE_DEBUG_INSTRUMENTATION
        ObjectPoolStorage& m_Storage;
#endif
    };

    /// Sorted by `Begin`: block addresses are not monotonic.
    std::vector<BlockDescriptor> m_Blocks;
    void* m_FreeHead = nullptr;
    /// The newest block's next never-used slot and the end of its slots; equal with no block.
    std::byte* m_Cursor = nullptr;
    std::byte* m_Limit = nullptr;
    std::byte* m_NewestBegin = nullptr;
    std::size_t m_SlotBytes;
    std::size_t m_BlockAlignment;
    std::size_t m_NewestSlotCount = 0;
    std::size_t m_LiveCount = 0;
    std::size_t m_ReservedBytes = 0;
    bool m_Destroying = false;
    Detail::StatsRecord m_Record;
#if GE_DEBUG_INSTRUMENTATION
    Detail::SerialUseCheck m_SerialUse;
#endif
};

inline void* ObjectPoolStorage::Take()
{
    const UseScope use(*this);
    assert(!m_Destroying && "New during the pool's destruction; create the object before or after the pool");
    void* slot = m_FreeHead;
    if (slot != nullptr)
    {
        // The link is the one word of a free slot left unpoisoned.
        std::memcpy(&m_FreeHead, slot, sizeof(void*));
    }
    else if (m_Cursor != m_Limit)
    {
        slot = m_Cursor;
        m_Cursor += m_SlotBytes;
    }
    else
    {
        slot = TakeFromNewBlock();
    }
#if defined(GE_MEMORY_ASAN)
    ASAN_UNPOISON_MEMORY_REGION(slot, m_SlotBytes);
#endif
#if GE_DEBUG_INSTRUMENTATION
    MarkLive(slot);
#endif
    ++m_LiveCount;
    return slot;
}

inline void ObjectPoolStorage::Return(void* slot)
{
    const UseScope use(*this);
#if GE_DEBUG_INSTRUMENTATION
    std::memset(slot, 0xDD, m_SlotBytes);
#endif
    std::memcpy(slot, &m_FreeHead, sizeof(void*));
    m_FreeHead = slot;
#if defined(GE_MEMORY_ASAN)
    ASAN_POISON_MEMORY_REGION(static_cast<std::byte*>(slot) + sizeof(void*), m_SlotBytes - sizeof(void*));
#endif
    --m_LiveCount;
}

} // namespace Detail

/// Fixed-size slots for objects of one type that need a stable address: `New` constructs an
/// object in a free slot, `Delete` destroys it and frees the slot for the next `New`.
///
///     Memory::ObjectPool<PathRequest> m_Requests{"MyGame.PathRequests"};
///     PathRequest* request = m_Requests.New(from, to, done);   // stable address until Delete
///     m_Requests.Delete(request);                              // runs ~PathRequest; the slot is reused next
///
/// For objects outside the ECS that are linked by raw pointer, handed to a library or
/// referenced from a callback. A game object that lives in the world is an entity; data
/// addressed by handle, whose storage may move, is a `GenerationalVector`.
///
/// - **Cost.** `New` and `Delete` are O(1): a freelist pop or push, or a step through the
///   newest block. Freed slots are reused last-in first-out. Nothing is written to the stats
///   per operation.
/// - **Lifetime.** An object lives until its `Delete` or the pool's destruction; its address
///   never changes. Blocks are never moved or freed before the pool's destruction, so the pool
///   never shrinks.
/// - **Growth.** The first block holds max(16, 16 KiB / slot) slots and each new block twice
///   the previous one, while a block's slots stay within `kArenaMaxBlockBytes` (4 MiB); a slot
///   larger than that gets a block of one slot. A slot is `sizeof(T)`, at least a pointer,
///   rounded up to `alignof(T)` (at least a pointer's alignment); any alignment is served.
/// - **Destruction.** The pool's destructor destroys every live object exactly once, in
///   address order. A destructor that runs then may `Delete` other objects of the same pool
///   (the deleted object is destroyed once, whichever of the two the walk reaches first), but
///   must not otherwise touch them, since one at a lower address is already destroyed, and
///   must not `New` from it (that object would never be destroyed).
/// - **Threading.** Externally serialized: one user at a time, on any thread, ordered by a
///   join, a lock or the frame (a system's member pool is fine). Debug and DebugFast assert
///   two users at once. Neither the constructor nor the destructor of `T` runs inside that
///   check, so they may use the same pool.
/// - **Failure.** `std::bad_alloc` when a block cannot be acquired; never null. A constructor
///   that throws returns its slot to the pool.
/// - **Stats.** Registered under `statsName` (copied, at most 47 characters) in
///   `Memory::SnapshotStats` as `StatsKind::ObjectPool`, written when a block is acquired:
///   the live objects' slot bytes as of that moment, the block bytes and the block count.
///   `HighWaterBytes` equals `Bytes`: every older slot is live when a block is acquired, and
///   the true peak since lies between it and `ReservedBytes`. `LiveCount()` is exact at any
///   time.
///
/// Debug and DebugFast fill a deleted object's slot with 0xDD and assert on a double `Delete`
/// and on a pointer that is not one of the pool's slots; in Release a double `Delete` is
/// undefined behaviour, as with `delete`. When no `New` follows it, the pool's destruction
/// aborts with "a slot was deleted twice; the freelist is a cycle"; after a `New` the same slot
/// can be handed out twice and nothing is reported. Under AddressSanitizer a free slot is poisoned except its
/// first pointer-sized word, which holds the freelist link, and a slot destroyed by the pool's
/// destruction is poisoned whole.
template <class T>
class ObjectPool
{
  public:
    /// Acquires nothing until the first `New`. `statsName` must not be empty.
    explicit ObjectPool(std::string_view statsName) : m_Storage(statsName, sizeof(T), alignof(T)) {}
    /// Destroys every live object once, then frees every block.
    ~ObjectPool() { m_Storage.DestroyLiveObjects(&DestroyObject); }

    // Live objects and the stats record point into the pool.
    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;
    ObjectPool(ObjectPool&&) = delete;
    ObjectPool& operator=(ObjectPool&&) = delete;

    /// Constructs a `T` in a free slot. The constructor may use this pool.
    template <class... Args>
    T* New(Args&&... args);

    /// Destroys `object`, which `New` of this pool returned and nothing has deleted since, and
    /// frees its slot; null does nothing, as with `delete`. The destructor may use this pool.
    void Delete(T* object);

    /// Objects created and not yet deleted.
    std::size_t LiveCount() const { return m_Storage.LiveCount(); }

  private:
    static void DestroyObject(void* object) { static_cast<T*>(object)->~T(); }

    /// `Delete` during the pool's destruction: destroys `object` unless the walk or an earlier
    /// `Delete` already has.
    void DeleteWhileDestroying(T* object);

    Detail::ObjectPoolStorage m_Storage;
};

template <class T>
template <class... Args>
T* ObjectPool<T>::New(Args&&... args)
{
    void* slot = m_Storage.Take();
    try
    {
        return ::new (slot) T(std::forward<Args>(args)...);
    }
    catch (...)
    {
        m_Storage.CheckLiveAndMarkFree(slot);
        m_Storage.Return(slot);
        throw;
    }
}

template <class T>
void ObjectPool<T>::Delete(T* object)
{
    if (object == nullptr)
        return;
    if (m_Storage.IsDestroying()) [[unlikely]]
    {
        DeleteWhileDestroying(object);
        return;
    }
#if GE_DEBUG_INSTRUMENTATION
    m_Storage.CheckLiveAndMarkFree(object);
#endif
    object->~T();
    m_Storage.Return(object);
}

template <class T>
void ObjectPool<T>::DeleteWhileDestroying(T* object)
{
    if (!m_Storage.ClaimWhileDestroying(object))
        return;
    object->~T();
    m_Storage.RetireWhileDestroying(object);
}

} // namespace GameEngine::Memory
