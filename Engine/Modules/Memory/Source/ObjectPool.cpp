// Memory::ObjectPool<T>'s untyped storage: block acquisition, the Debug bit writes and checks,
// and the pool's destruction. `Take` and `Return` are inline in the header; everything here
// runs once per block, only in Debug and DebugFast, or only while the pool is destroyed. None
// of it is a template (decision 67 of the allocator design: a template's out-of-line work
// goes through non-template functions, which Engine.dll's generated export table carries).

#include "Memory/ObjectPool.h"

#include "Memory/Arena.h"

#include "StatsRegistry.h"
#include "Types/FalseSharing.h"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <new>

namespace GameEngine::Memory::Detail
{
namespace
{

/// Slots in the first block, unless the cap allows fewer.
constexpr std::size_t kFirstBlockMinimumSlots = 16;
/// Slot bytes the first block aims for when the slot is small.
constexpr std::size_t kFirstBlockSlotBytes = 16 * 1024;
constexpr std::size_t kBitsPerWord = 64;

std::size_t RoundUp(std::size_t value, std::size_t multiple)
{
    return (value + multiple - 1) / multiple * multiple;
}

/// The bitmap after a block's slots: one bit per slot, in whole 64-bit words.
std::size_t BitmapBytes(std::size_t slotCount)
{
    return RoundUp(slotCount, kBitsPerWord) / 8;
}

std::size_t BitmapWords(std::size_t slotCount)
{
    return RoundUp(slotCount, kBitsPerWord) / kBitsPerWord;
}

bool TestBit(const std::uint64_t* bitmap, std::size_t slot)
{
    return (bitmap[slot / kBitsPerWord] >> (slot % kBitsPerWord)) & 1u;
}

void SetBit(std::uint64_t* bitmap, std::size_t slot)
{
    bitmap[slot / kBitsPerWord] |= std::uint64_t{1} << (slot % kBitsPerWord);
}

void ClearBit(std::uint64_t* bitmap, std::size_t slot)
{
    bitmap[slot / kBitsPerWord] &= ~(std::uint64_t{1} << (slot % kBitsPerWord));
}

} // namespace

ObjectPoolStorage::ObjectPoolStorage(std::string_view statsName, std::size_t objectBytes, std::size_t objectAlignment)
    : m_SlotBytes(RoundUp(std::max(objectBytes, sizeof(void*)), std::max(objectAlignment, alignof(void*)))),
      m_BlockAlignment(std::max(objectAlignment, kFalseSharingSeparation))
{
    assert(!statsName.empty() && "an ObjectPool needs a stats name; name it after its owner, as 'Area.What'");
    RegisterStats(m_Record, statsName, StatsKind::ObjectPool, Residency::Cpu);
}

ObjectPoolStorage::~ObjectPoolStorage()
{
    UnregisterStats(m_Record);
    for (const BlockDescriptor& block : m_Blocks)
    {
        const std::size_t blockBytes = block.SlotCount * m_SlotBytes + BitmapBytes(block.SlotCount);
#if defined(GE_MEMORY_ASAN)
        ASAN_UNPOISON_MEMORY_REGION(block.Begin, blockBytes);
#endif
        ::operator delete(block.Begin, blockBytes, std::align_val_t{m_BlockAlignment});
    }
}

void* ObjectPoolStorage::TakeFromNewBlock()
{
    const std::size_t capSlots = std::max<std::size_t>(1, kArenaMaxBlockBytes / m_SlotBytes);
    const std::size_t growthSlots = m_Blocks.empty()
                                        ? std::max(kFirstBlockMinimumSlots, kFirstBlockSlotBytes / m_SlotBytes)
                                        : m_NewestSlotCount * 2;
    const std::size_t slotCount = std::min(growthSlots, capSlots);
    const std::size_t slotBytes = slotCount * m_SlotBytes;
    const std::size_t blockBytes = slotBytes + BitmapBytes(slotCount);

    // The descriptor's place first, so a failure to grow the vector cannot leak the block.
    if (m_Blocks.size() == m_Blocks.capacity())
        m_Blocks.reserve(std::max<std::size_t>(8, m_Blocks.capacity() * 2));
    auto* begin = static_cast<std::byte*>(::operator new(blockBytes, std::align_val_t{m_BlockAlignment}));
    auto* bitmap = reinterpret_cast<std::uint64_t*>(begin + slotBytes);
    std::fill_n(bitmap, BitmapWords(slotCount), std::uint64_t{0});
#if defined(GE_MEMORY_ASAN)
    // The slots until the bump hands them out; the bitmap stays readable.
    ASAN_POISON_MEMORY_REGION(begin, slotBytes);
#endif
    const BlockDescriptor descriptor{begin, static_cast<std::uint32_t>(slotCount), bitmap};
    const auto place = std::upper_bound(m_Blocks.begin(), m_Blocks.end(), begin,
                                        [](const std::byte* address, const BlockDescriptor& block) {
                                            return address < block.Begin;
                                        });
    m_Blocks.insert(place, descriptor);

    m_NewestBegin = begin;
    m_NewestSlotCount = slotCount;
    m_Cursor = begin + m_SlotBytes;
    m_Limit = begin + slotBytes;
    m_ReservedBytes += blockBytes;
    // Every slot of the older blocks is live (the freelist is empty), and so is the one taken.
    PublishStats(m_LiveCount + 1);
    return begin;
}

std::size_t ObjectPoolStorage::UsedSlots(const BlockDescriptor& block) const
{
    if (block.Begin != m_NewestBegin)
        return block.SlotCount;
    return static_cast<std::size_t>(m_Cursor - block.Begin) / m_SlotBytes;
}

ObjectPoolStorage::SlotLocation ObjectPoolStorage::Locate(const void* slot) const
{
    const auto* address = static_cast<const std::byte*>(slot);
    const auto after = std::upper_bound(m_Blocks.begin(), m_Blocks.end(), address,
                                        [](const std::byte* value, const BlockDescriptor& block) {
                                            return value < block.Begin;
                                        });
    assert(after != m_Blocks.begin() && "this pointer did not come from this pool");
    const BlockDescriptor& block = *(after - 1);
    const auto offset = static_cast<std::size_t>(address - block.Begin);
    assert(offset < block.SlotCount * m_SlotBytes && offset % m_SlotBytes == 0 &&
           "this pointer did not come from this pool");
    return {static_cast<std::size_t>(after - 1 - m_Blocks.begin()), offset / m_SlotBytes};
}

void ObjectPoolStorage::MarkLive(void* slot)
{
    const SlotLocation location = Locate(slot);
    SetBit(m_Blocks[location.Block].Bitmap, location.Slot);
}

void ObjectPoolStorage::CheckLiveAndMarkFree([[maybe_unused]] void* slot)
{
#if GE_DEBUG_INSTRUMENTATION
    const UseScope use(*this);
    const SlotLocation location = Locate(slot);
    std::uint64_t* bitmap = m_Blocks[location.Block].Bitmap;
    assert(TestBit(bitmap, location.Slot) && "this object was already returned to the pool");
    ClearBit(bitmap, location.Slot);
#endif
}

bool ObjectPoolStorage::ClaimWhileDestroying(void* slot)
{
    const UseScope use(*this);
    const SlotLocation location = Locate(slot);
    std::uint64_t* bitmap = m_Blocks[location.Block].Bitmap;
    if (!TestBit(bitmap, location.Slot))
        return false;
    ClearBit(bitmap, location.Slot);
    --m_LiveCount;
    return true;
}

void ObjectPoolStorage::RetireWhileDestroying([[maybe_unused]] void* slot)
{
#if GE_DEBUG_INSTRUMENTATION
    std::memset(slot, 0xDD, m_SlotBytes);
#endif
#if defined(GE_MEMORY_ASAN)
    ASAN_POISON_MEMORY_REGION(slot, m_SlotBytes);
#endif
}

void ObjectPoolStorage::CheckBitsAgreeWithFreelist() const
{
#if GE_DEBUG_INSTRUMENTATION
    std::size_t freeCount = 0;
    for (void* slot = m_FreeHead; slot != nullptr;)
    {
        const SlotLocation location = Locate(slot);
        assert(!TestBit(m_Blocks[location.Block].Bitmap, location.Slot) &&
               "a slot on the pool's freelist is marked live");
        ++freeCount;
        std::memcpy(&slot, slot, sizeof(void*));
    }
    std::size_t usedSlots = 0;
    std::size_t liveBits = 0;
    for (const BlockDescriptor& block : m_Blocks)
    {
        usedSlots += UsedSlots(block);
        for (std::size_t word = 0; word < BitmapWords(block.SlotCount); ++word)
            liveBits += static_cast<std::size_t>(std::popcount(block.Bitmap[word]));
    }
    assert(liveBits == m_LiveCount && "the pool's live bits disagree with its live count");
    assert(usedSlots == m_LiveCount + freeCount && "the pool's used slots are not its live and free slots");
#endif
}

void ObjectPoolStorage::DeriveLiveSet()
{
    std::size_t usedSlots = 0;
    for (const BlockDescriptor& block : m_Blocks)
    {
        const std::size_t used = UsedSlots(block);
        usedSlots += used;
        const std::size_t fullWords = used / kBitsPerWord;
        std::fill_n(block.Bitmap, fullWords, ~std::uint64_t{0});
        std::fill(block.Bitmap + fullWords, block.Bitmap + BitmapWords(block.SlotCount), std::uint64_t{0});
        if (used % kBitsPerWord != 0)
            block.Bitmap[fullWords] = (std::uint64_t{1} << (used % kBitsPerWord)) - 1;
    }
    // A freelist holds at most the used slots. Release keeps no per-slot bit, so a double
    // Delete there links one slot in twice and the freelist becomes a cycle.
    std::size_t links = 0;
    for (void* slot = m_FreeHead; slot != nullptr;)
    {
        if (++links > usedSlots)
        {
            std::fprintf(stderr, "ObjectPool: a slot was deleted twice; the freelist is a cycle\n");
            std::abort();
        }
        const SlotLocation location = Locate(slot);
        ClearBit(m_Blocks[location.Block].Bitmap, location.Slot);
        std::memcpy(&slot, slot, sizeof(void*));
    }
}

void ObjectPoolStorage::DestroyLiveObjects(void (*destroy)(void*)) noexcept
{
    CheckBitsAgreeWithFreelist();
    DeriveLiveSet();
    m_Destroying = true;
    // By index: a Release New from a destructor (forbidden, asserted in Debug) may insert a
    // descriptor, which at worst revisits a block whose live bits are already clear.
    for (std::size_t blockIndex = 0; blockIndex < m_Blocks.size(); ++blockIndex)
    {
        for (std::size_t slotIndex = 0; slotIndex < UsedSlots(m_Blocks[blockIndex]); ++slotIndex)
        {
            const BlockDescriptor& block = m_Blocks[blockIndex];
            if (!TestBit(block.Bitmap, slotIndex))
                continue;
            // Cleared first: a Delete of this object from a sibling's destructor is then a no-op.
            ClearBit(block.Bitmap, slotIndex);
            --m_LiveCount;
            void* slot = block.Begin + slotIndex * m_SlotBytes;
            destroy(slot);
            RetireWhileDestroying(slot);
        }
    }
}

void ObjectPoolStorage::PublishStats(std::size_t liveCount)
{
    const std::uint64_t liveBytes = static_cast<std::uint64_t>(liveCount) * m_SlotBytes;
    m_Record.Bytes.store(liveBytes, std::memory_order_relaxed);
    m_Record.ReservedBytes.store(m_ReservedBytes, std::memory_order_relaxed);
    m_Record.Blocks.store(m_Blocks.size(), std::memory_order_relaxed);
    if (liveBytes > m_Record.HighWaterBytes.load(std::memory_order_relaxed))
        m_Record.HighWaterBytes.store(liveBytes, std::memory_order_relaxed);
}

} // namespace GameEngine::Memory::Detail
