#pragma once

#include "PageStreaming/PageAddress.h"
#include "PageStreaming/PageStoreFormat.h"
#include "Types/Types.h"

#include <algorithm>
#include <cstddef>
#include <span>

namespace GameEngine::PageStreaming
{

/// Where a page of a stream stands on the CPU.
enum class CpuPageState : uint8
{
    Missing,  ///< not loaded yet, in flight, or its load failed and waits for a retry
    Resident, ///< its samples are on the CPU
    Absent,   ///< not in its source: its parent is the finest data there
};

/// One page's entry in a stream's CPU page directory.
struct CpuPageEntry
{
    const uint8* Bytes = nullptr; ///< its CpuPageDirectory::PageBytes bytes while Resident, else null
    CpuPageState State = CpuPageState::Missing;
};

/// A stream's CPU pages as a directory: one entry per page of its pyramid, in the order of the
/// store's index (face, level, row, column), so a CPU reader finds a page with one array read, as
/// the GPU finds it in its page table. Its size is the pyramid's page count (16 bytes a page), not
/// the CPU budget. The directory is valid until the stream is configured again or forgotten; a
/// page landed or fetched meanwhile shows in it at once.
///
/// It also records which entries changed, for a reader that keeps answers across frames: each
/// landing, absence and drop is the change numbered ChangesEnd() at that moment, held at
/// Changes[number % Changes.size()]. The record is a ring the stream allocates when it is
/// configured and never moves: it keeps the latest Changes.size() changes (ChangesBegin() moves
/// past the older ones), and a restarted stream starts a new record under a new Epoch. Like the
/// entries, the record is read as it is now: a change made after the directory was taken (an
/// Update, or a FetchCpuPage) shows in it at once.
struct CpuPageDirectory
{
    std::span<const PageStoreLevel> Levels;
    std::span<const CpuPageEntry> Entries;
    uint32 EntriesPerFace = 0;
    uint64 Epoch = 0; ///< the stream's configuration; 0 when the directory is empty
    std::span<const PageAddress> Changes; ///< the ring: change number n at Changes[n % Changes.size()]
    const uint64* ChangeCount = nullptr;  ///< the stream's changes so far; null when the directory is empty
    uint32 PageBytes = 0;                 ///< every page's bytes: its source's PageSource::PayloadBytes

    /// The number of the next change: a reader's place in the record.
    uint64 ChangesEnd() const { return ChangeCount ? *ChangeCount : 0u; }
    /// The number of the oldest change the record still holds.
    uint64 ChangesBegin() const
    {
        const uint64 end = ChangesEnd();
        return end > Changes.size() ? end - Changes.size() : 0u;
    }

    /// The entry index of `address`, or Entries.size() when the address is outside the pyramid.
    std::size_t Index(const PageAddress& address) const
    {
        if (address.Level >= Levels.size())
            return Entries.size();
        const PageStoreLevel& level = Levels[address.Level];
        if (address.X >= level.PagesX || address.Z >= level.PagesZ)
            return Entries.size();
        const std::size_t index = static_cast<std::size_t>(address.Face) * EntriesPerFace + level.FirstEntry +
                                  static_cast<std::size_t>(address.Z) * level.PagesX + address.X;
        return std::min(index, Entries.size());
    }

    /// Where page `address` stands on the CPU; its bytes go to `outBytes` when it is Resident (a
    /// height page's heights through HeightSamples). An address outside the pyramid is Missing.
    CpuPageState Page(const PageAddress& address, std::span<const uint8>& outBytes) const
    {
        const std::size_t index = Index(address);
        if (index == Entries.size())
            return CpuPageState::Missing;
        const CpuPageEntry& entry = Entries[index];
        if (entry.State == CpuPageState::Resident)
            outBytes = std::span<const uint8>(entry.Bytes, PageBytes);
        return entry.State;
    }
};

} // namespace GameEngine::PageStreaming
