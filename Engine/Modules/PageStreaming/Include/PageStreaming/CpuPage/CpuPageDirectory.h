#pragma once

#include "PageStreaming/CpuPage/CpuPageEntries.h"
#include "PageStreaming/PageAddress.h"
#include "Types/Types.h"

#include <cassert>
#include <span>

namespace GameEngine::PageStreaming
{

/// A stream's CPU pages: an entry for each page the CPU holds (Resident) or knows is not in its source
/// (Absent), in a hash table of 32 to 64 bytes a page (CpuPageEntries); every other page is Missing.
/// Valid until the stream is configured again or forgotten; a page landed or fetched meanwhile shows at once.
struct CpuPageDirectory
{
    const CpuPageEntries* Entries = nullptr; ///< the stream's entries; null when the directory is empty
    uint64 Epoch = 0; ///< the stream's configuration; 0 when the directory is empty
    /// Each landing, absence and drop, for a reader that keeps answers across frames: change n is at
    /// Changes[n % Changes.size()] in a ring that never moves and keeps the changes from ChangesBegin()
    /// to ChangesEnd(). A restarted stream starts a new record under a new Epoch.
    std::span<const PageAddress> Changes;
    const uint64* ChangeCount = nullptr;  ///< the stream's changes so far; null when the directory is empty
    uint32 PageBytes = 0;                 ///< every page's bytes: its source's PayloadBytes

    /// The entry key of `address`: face in bits 56 to 63, level in 48 to 55, Z in 24 to 47 and X in
    /// 0 to 23. A level has at most 2^23 pages a side (a store's 2^30 samples a side), so bit 23 is
    /// clear in every key.
    static uint64 Key(const PageAddress& address)
    {
        [[maybe_unused]] constexpr uint32 kMaxPagesPerAxis = 1u << 23u;
        assert(address.X < kMaxPagesPerAxis && address.Z < kMaxPagesPerAxis && "a page past a store's extent");
        return (static_cast<uint64>(address.Face) << 56u) | (static_cast<uint64>(address.Level) << 48u) |
               (static_cast<uint64>(address.Z) << 24u) | address.X;
    }

    /// True when the stream is not configured or had no CPU request yet: every page is Missing.
    bool Empty() const { return Entries == nullptr; }

    /// The number of the next change: a reader's place in the record.
    uint64 ChangesEnd() const { return ChangeCount ? *ChangeCount : 0u; }
    /// The number of the oldest change the record still holds.
    uint64 ChangesBegin() const
    {
        const uint64 end = ChangesEnd();
        return end > Changes.size() ? end - Changes.size() : 0u;
    }

    /// The entry of page `address`: where it stands on the CPU (State) and its bytes (BytesOf). A page
    /// with no entry is Missing.
    CpuPageEntry Page(const PageAddress& address) const
    {
        return Entries ? Entries->Entry(Key(address)) : CpuPageEntry::Missing;
    }

    /// The page's bytes while `entry` is Resident (a height page's heights through HeightSamples),
    /// else empty.
    std::span<const uint8> BytesOf(CpuPageEntry entry) const
    {
        const uint8* bytes = Bytes(entry);
        return bytes ? std::span<const uint8>(bytes, PageBytes) : std::span<const uint8>();
    }
};

} // namespace GameEngine::PageStreaming
