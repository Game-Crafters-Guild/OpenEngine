// Memory::Arena's out-of-line paths: block acquisition, destructor records, Rewind and Reset.
// The bump itself is inline in the header; everything here runs at most once per block, per
// recorded destructor, or per Rewind and Reset. None of it is a template (decision 67 of the
// allocator design: a template's out-of-line work goes through non-template functions, which
// Engine.dll's generated export table carries).

#include "Memory/Arena.h"

#include "StatsRegistry.h"
#include "Types/FalseSharing.h"

#include <algorithm>
#include <cassert>
#include <cstring>

namespace GameEngine::Memory
{
namespace
{

/// Block bytes are aligned to the false-sharing separation, so a block never shares a line
/// with another allocation and every alignment up to it costs no padding at a block's start.
constexpr std::size_t kBlockAlignment = kFalseSharingSeparation;

std::byte* AcquireBlockMemory(std::size_t bytes)
{
    auto* data = static_cast<std::byte*>(::operator new(bytes, std::align_val_t{kBlockAlignment}));
#if defined(GE_MEMORY_ASAN)
    ASAN_POISON_MEMORY_REGION(data, bytes);
#endif
    return data;
}

void ReleaseBlockMemory(std::byte* data, [[maybe_unused]] std::size_t bytes)
{
#if defined(GE_MEMORY_ASAN)
    ASAN_UNPOISON_MEMORY_REGION(data, bytes);
#endif
    ::operator delete(data, std::align_val_t{kBlockAlignment});
}

std::size_t RoundUpToBlockAlignment(std::size_t bytes)
{
    return (bytes + (kBlockAlignment - 1)) & ~(kBlockAlignment - 1);
}

/// The size of a block that serves `bytes` at `alignment` at any address: the worst-case
/// padding plus the request.
std::size_t BlockBytesFor(std::size_t bytes, std::size_t alignment)
{
    const std::size_t padding = alignment > kBlockAlignment ? alignment : 0;
    if (bytes > std::numeric_limits<std::size_t>::max() - padding - kBlockAlignment)
        throw std::bad_alloc();
    return RoundUpToBlockAlignment(bytes + padding);
}

} // namespace

Arena::Arena(std::string_view statsName, std::size_t firstBlockBytes)
    : m_FirstBlockBytes(RoundUpToBlockAlignment(firstBlockBytes != 0 ? firstBlockBytes : kArenaFirstBlockBytes))
{
    assert(!statsName.empty() && "an Arena needs a stats name; name it after its owner, as 'Area.What'");
    Detail::RegisterStats(m_Record, statsName, StatsKind::Arena, Residency::Cpu);
}

Arena::Arena(Detail::ScratchArenaTag, std::string_view statsName, std::uint64_t threadId)
    : m_FirstBlockBytes(kArenaFirstBlockBytes)
{
    m_Record.ThreadId = threadId;
    Detail::RegisterStats(m_Record, statsName, StatsKind::Scratch, Residency::Cpu);
}

Arena::~Arena()
{
    RunDestructorsAbove(0);
    Detail::UnregisterStats(m_Record);
    for (const Block& block : m_Blocks)
        ReleaseBlockMemory(block.Data, block.Capacity);
}

void* Arena::ArenaResource::do_allocate(std::size_t bytes, std::size_t alignment)
{
    return m_Arena.Allocate(bytes, alignment);
}

void* Arena::AllocateFromNextBlock(std::size_t bytes, std::size_t alignment)
{
    if (!m_Blocks.empty())
    {
        Block& current = m_Blocks[m_Current];
        current.Used = m_Cursor - reinterpret_cast<std::uintptr_t>(current.Data);
        m_PeakSinceReset = std::max(m_PeakSinceReset, m_UsedBeforeCurrent + current.Used);
        // Blocks after the current one are empty: a Rewind empties every block it passes.
        for (std::size_t next = m_Current + 1; next < m_Blocks.size(); ++next)
        {
            const std::uintptr_t data = reinterpret_cast<std::uintptr_t>(m_Blocks[next].Data);
            const std::uintptr_t limit = data + m_Blocks[next].Capacity;
            const std::uintptr_t aligned = (data + (alignment - 1)) & ~static_cast<std::uintptr_t>(alignment - 1);
            if (aligned > limit || bytes > limit - aligned)
                continue;
            for (std::size_t skipped = m_Current; skipped < next; ++skipped)
                m_UsedBeforeCurrent += m_Blocks[skipped].Used;
            m_Current = next;
            m_Cursor = data;
            m_Limit = limit;
            return TryBump(bytes, alignment);
        }
    }

    const std::size_t growthStep =
        m_Blocks.empty() ? m_FirstBlockBytes : std::min(m_Blocks.back().Capacity * 2, kArenaMaxBlockBytes);
    const std::size_t blockBytes = std::max(growthStep, BlockBytesFor(bytes, alignment));
    if (m_Blocks.size() == m_Blocks.capacity())
        m_Blocks.reserve(std::max<std::size_t>(4, m_Blocks.capacity() * 2));
    std::byte* data = AcquireBlockMemory(blockBytes);
    for (std::size_t passed = m_Current; passed < m_Blocks.size(); ++passed)
        m_UsedBeforeCurrent += m_Blocks[passed].Used;
    m_Blocks.push_back({data, blockBytes, 0});
    m_Current = m_Blocks.size() - 1;
    m_Cursor = reinterpret_cast<std::uintptr_t>(data);
    m_Limit = m_Cursor + blockBytes;
    void* memory = TryBump(bytes, alignment);
    PublishStats();
    return memory;
}

void* Arena::AllocateArrayBytes(std::size_t count, std::size_t elementBytes, std::size_t alignment)
{
    if (elementBytes != 0 && count > std::numeric_limits<std::size_t>::max() / elementBytes)
        throw std::bad_array_new_length();
    return Allocate(count * elementBytes, alignment);
}

void Arena::ReserveDestructorRecord()
{
    const UseScope use(*this);
    if (m_Destructors.size() == m_Destructors.capacity())
        m_Destructors.reserve(std::max<std::size_t>(64, m_Destructors.capacity() * 2));
}

void Arena::RecordDestructor(void* object, void (*destroy)(void*))
{
    bool recorded = false;
    {
        const UseScope use(*this);
        // The slot reserved before construction may have been taken by a New<U> inside the
        // constructor, so this push can still grow the vector.
        try
        {
            m_Destructors.push_back({object, destroy});
            recorded = true;
        }
        catch (const std::bad_alloc&)
        {
        }
        if (recorded)
        {
            // The owner is the only writer of the bounds (the arena is externally
            // serialized), so a compare and a plain store suffice. DestructorLow == 0 means
            // "holds none" to the image-pin query, which loads it with acquire: the first
            // record stores High before Low, and Low with release, so a query that sees a
            // non-zero Low also sees the High stored with it. Widening an interval the query
            // already sees stays relaxed.
            const auto address = reinterpret_cast<std::uint64_t>(destroy);
            Detail::StatsRecord& record = m_Record;
            const std::uint64_t low = record.DestructorLow.load(std::memory_order_relaxed);
            if (low == 0)
            {
                record.DestructorHigh.store(address, std::memory_order_relaxed);
                record.DestructorLow.store(address, std::memory_order_release);
            }
            else
            {
                if (address < low)
                    record.DestructorLow.store(address, std::memory_order_relaxed);
                if (address > record.DestructorHigh.load(std::memory_order_relaxed))
                    record.DestructorHigh.store(address, std::memory_order_relaxed);
            }
        }
    }
    if (!recorded)
    {
        // Destroyed rather than leaked, outside the hold: the destructor may use the arena.
        destroy(object);
        throw std::bad_alloc();
    }
}

void Arena::RunDestructorsAbove(std::uint32_t count)
{
    // Not held across the destructor itself, which may allocate from this arena.
    for (;;)
    {
        DestructorRecord record;
        {
            const UseScope use(*this);
            if (m_Destructors.size() <= count)
                return;
            record = m_Destructors.back();
            m_Destructors.pop_back();
        }
        record.Destroy(record.Object);
    }
}

void Arena::ClearDestructorBoundsIfNoneRecorded()
{
    // Only after the destructors have run: cleared earlier, an unload decision taken on
    // another thread could unmap code a destructor is still executing. Both stores release,
    // and the query acquires both, so a query that sees either cleared bound also sees the
    // destructors as having run. The owner is the only writer and stores `DestructorLow` last,
    // so a zero `DestructorLow` means both bounds are already clear and nothing is stored: a
    // Rewind or Reset that follows no recorded destructor pays no store-release.
    if (!m_Destructors.empty() || m_Record.DestructorLow.load(std::memory_order_relaxed) == 0)
        return;
    m_Record.DestructorHigh.store(0, std::memory_order_release);
    m_Record.DestructorLow.store(0, std::memory_order_release);
}

void Arena::Rewind(Marker marker)
{
    RewindPublishing(marker, true);
}

void Arena::RewindPublishing(Marker marker, bool publishStats)
{
    assert(marker.DestructorCount <= m_Destructors.size() &&
           "Rewind to a marker taken before a later Rewind or Reset; take the marker again");
    RunDestructorsAbove(marker.DestructorCount);

    const UseScope use(*this);
    if (!m_Blocks.empty())
    {
        assert(marker.Block <= m_Current && "Rewind to a marker taken before a later Rewind or Reset");
        Block& current = m_Blocks[m_Current];
        current.Used = m_Cursor - reinterpret_cast<std::uintptr_t>(current.Data);
        m_PeakSinceReset = std::max(m_PeakSinceReset, m_UsedBeforeCurrent + current.Used);
        for (std::size_t block = marker.Block + 1; block <= m_Current; ++block)
        {
            RetireRange(m_Blocks[block].Data, m_Blocks[block].Used);
            m_Blocks[block].Used = 0;
        }
        Block& target = m_Blocks[marker.Block];
        assert(marker.Offset <= target.Used && "Rewind to a marker taken before a later Rewind or Reset");
        RetireRange(target.Data + marker.Offset, target.Used - marker.Offset);
        target.Used = marker.Offset;
        m_UsedBeforeCurrent = 0;
        for (std::size_t block = 0; block < marker.Block; ++block)
            m_UsedBeforeCurrent += m_Blocks[block].Used;
        m_Current = marker.Block;
        m_Cursor = reinterpret_cast<std::uintptr_t>(target.Data) + marker.Offset;
        m_Limit = reinterpret_cast<std::uintptr_t>(target.Data) + target.Capacity;
    }
    ClearDestructorBoundsIfNoneRecorded();
    if (publishStats)
        PublishStats();
}

void Arena::Reset()
{
    ResetPublishing(true);
}

void Arena::ResetPublishing(bool publishStats)
{
    RunDestructorsAbove(0);

    const UseScope use(*this);
    if (!m_Blocks.empty())
    {
        Block& current = m_Blocks[m_Current];
        current.Used = m_Cursor - reinterpret_cast<std::uintptr_t>(current.Data);
        m_PeakSinceReset = std::max(m_PeakSinceReset, m_UsedBeforeCurrent + current.Used);
        if (m_Blocks.size() > 1)
        {
            // One block of the high-water size, acquired before the chain is freed so a failed
            // acquisition leaves the arena whole.
            const std::size_t blockBytes = RoundUpToBlockAlignment(std::max(m_PeakSinceReset, m_FirstBlockBytes));
            std::byte* data = AcquireBlockMemory(blockBytes);
            for (const Block& block : m_Blocks)
                ReleaseBlockMemory(block.Data, block.Capacity);
            m_Blocks.clear();
            m_Blocks.push_back({data, blockBytes, 0});
        }
        else
        {
            RetireRange(current.Data, current.Used);
            current.Used = 0;
        }
        m_Current = 0;
        m_Cursor = reinterpret_cast<std::uintptr_t>(m_Blocks[0].Data);
        m_Limit = m_Cursor + m_Blocks[0].Capacity;
    }
    m_UsedBeforeCurrent = 0;
    ClearDestructorBoundsIfNoneRecorded();
    // Published before the peak is cleared, so the high water takes this Reset's peak.
    if (publishStats)
        PublishStats();
    m_PeakSinceReset = 0;
}

std::size_t Arena::PeakBytesSinceReset() const
{
    return std::max(m_PeakSinceReset, BytesUsed());
}

void Arena::ReleaseBlocks(std::size_t firstBlockBytes)
{
    const UseScope use(*this);
    assert(BytesUsed() == 0 && m_Destructors.empty() && "ReleaseBlocks on an arena in use; Reset it first");
    for (const Block& block : m_Blocks)
        ReleaseBlockMemory(block.Data, block.Capacity);
    m_Blocks.clear();
    m_Current = 0;
    m_Cursor = 1;
    m_Limit = 0;
    m_UsedBeforeCurrent = 0;
    m_PeakSinceReset = 0;
    m_FirstBlockBytes = RoundUpToBlockAlignment(firstBlockBytes);
    PublishStats();
}

std::size_t Arena::BytesUsed() const
{
    if (m_Blocks.empty())
        return 0;
    return m_UsedBeforeCurrent + (m_Cursor - reinterpret_cast<std::uintptr_t>(m_Blocks[m_Current].Data));
}

std::size_t Arena::BytesReserved() const
{
    std::size_t reserved = 0;
    for (const Block& block : m_Blocks)
        reserved += block.Capacity;
    return reserved;
}

void Arena::PublishStats()
{
    Detail::StatsRecord& record = m_Record;
    const std::uint64_t used = BytesUsed();
    record.Bytes.store(used, std::memory_order_relaxed);
    record.ReservedBytes.store(BytesReserved(), std::memory_order_relaxed);
    record.Blocks.store(m_Blocks.size(), std::memory_order_relaxed);
    // The peak since the last Reset covers what a Rewind or Reset has just taken back.
    const std::uint64_t peak = std::max<std::uint64_t>(used, m_PeakSinceReset);
    if (peak > record.HighWaterBytes.load(std::memory_order_relaxed))
        record.HighWaterBytes.store(peak, std::memory_order_relaxed);
}

} // namespace GameEngine::Memory
