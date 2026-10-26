#pragma once

#include "PageStreaming/PageAddress.h"
#include "PageStreaming/PageStoreFormat.h"
#include "Types/Types.h"

#include <span>
#include <vector>

// The page table of one field (design page-streaming-planets 3.4): the cache slot of every resident
// page of every level, in the words the GPU reads at binding 22 (CBTLayout.h) and the CPU mirror
// walks (PagedHeightSampler).
//
// A field whose dense table fits the budget keeps one tier: an entry for every page of every level,
// level-major, row-major. A larger field keeps that dense tier only from its top level down to the
// floor level whose cumulative entries fit a quarter of the budget. Below the floor, blocks hold
// one page's descendants for up to six levels, from a mid pool and a leaf pool,
// and exist only while a page under them is resident. A lookup reads the floor's directory, then
// each tier's block (its entries, or its directory of the next tier's blocks): at most three
// dependent reads to 12 levels below the floor. cbt_page.glsl's CBT_PageTableEntry is the same walk.
//
// The words: one descriptor of four words per tier (the root level, the block's levels, the block's
// words, the pool's first word), the dense levels, the floor directory (one block index per floor
// page, kNoPageBlock in PageTable.cpp when none), then the pools. A block holds its levels' entries,
// level k below its root at ((4^k - 4) / 3) on a 2^k x 2^k grid, then, when a tier follows, a
// directory of 4^levels block indices, one per page of its deepest level. A dense table has no
// descriptor, directory or pool: its words are the entries alone.
namespace GameEngine::TerrainECS
{

/// What one field's page table may spend.
struct PageTableBudget
{
    uint64 DenseWords = 0; ///< a table whose dense words (binding-22 header, fades, entries) fit keeps one tier
    uint32 MidBlocks = 0;  ///< the blocks of each tier but the last
    uint32 LeafBlocks = 0; ///< the blocks of the last tier
};

class PageTable
{
public:
    /// An empty table (no page resident) for a field of samplesX x samplesZ level-0 samples and a
    /// cache of slotCount slots.
    void Reset(uint32 samplesX, uint32 samplesZ, uint32 slotCount, const PageTableBudget& budget);

    /// The entry of a page: PackPageEntry(slot, level), or kNoPage.
    uint32 Entry(uint32 level, uint32 pageX, uint32 pageZ) const;
    /// Sets the entry of a page. A resident page below the floor whose pool is full waits, reading
    /// kNoPage (its first resident ancestor answers), until a block frees.
    void SetEntry(uint32 level, uint32 pageX, uint32 pageZ, uint32 entry);
    void SetSlotFade(uint32 slot, float32 fade) { m_SlotFade[slot] = fade; }

    /// Appends every page with an entry.
    void AppendResident(std::vector<PageStreaming::PageAddress>& out) const;
    /// Moves out the waiting pages that took their entry since the last call.
    void TakeLateEntries(std::vector<PageStreaming::PageAddress>& out);

    std::span<const PageStreaming::PageStoreLevel> Levels() const { return m_Levels; }
    /// Each level's first word: its dense entries, or its offset inside a block.
    std::span<const uint32> LevelOffsets() const { return m_LevelOffsets; }
    std::span<const uint32> Words() const { return m_Words; }
    std::span<const float32> SlotFades() const { return m_SlotFade; }
    /// The finest dense level: 0 for a dense table.
    uint32 FloorLevel() const { return m_FloorLevel; }
    uint32 DirectoryOffset() const { return m_DirectoryOffset; }
    /// The tiers of blocks below the floor: 0 for a dense table.
    uint32 TierCount() const { return static_cast<uint32>(m_Tiers.size()); }

private:
    /// One tier of blocks below the dense tier.
    struct Tier
    {
        uint32 RootLevel = 0;  ///< the level of the page a block hangs under
        uint32 Levels = 0;     ///< RootLevel - 1 down to RootLevel - Levels
        uint32 BlockWords = 0; ///< the levels' entries, then the next tier's directory when one follows
        uint32 PoolOffset = 0; ///< the pool's first word
        uint32 Blocks = 0;
    };

    struct Pool
    {
        std::vector<uint32> Free;
        std::vector<uint32> Residents; ///< per block: its resident entries and its allocated child blocks
        std::vector<uint32> Owner;     ///< per block: the directory word that points at it
        std::vector<uint32> Parent;    ///< per block: its parent block in the tier above (kNoPageBlock: the floor's)
        std::vector<uint32> RootX;     ///< per block: the page it hangs under, at its tier's root level
        std::vector<uint32> RootZ;
    };

    struct Waiting
    {
        PageStreaming::PageAddress Address;
        uint32 Entry = 0;
    };

    uint32 DenseWord(uint32 level, uint32 pageX, uint32 pageZ) const;
    uint32 FloorDirectoryWord(uint32 level, uint32 pageX, uint32 pageZ) const;
    /// The word of `tier`'s block `block` on the walk to a page: its entry when the page's level is in
    /// the block, else the next tier's directory word. (rootX, rootZ) is the page at the tier's root
    /// level that the block hangs under.
    uint32 BlockWord(const Tier& tier, uint32 block, uint32 rootX, uint32 rootZ, uint32 level, uint32 pageX,
                     uint32 pageZ) const;
    bool Insert(uint32 level, uint32 pageX, uint32 pageZ, uint32 entry);
    void Remove(uint32 level, uint32 pageX, uint32 pageZ);
    void Release(uint32 tier, uint32 block);
    void RetryWaiting();

    std::vector<PageStreaming::PageStoreLevel> m_Levels;
    std::vector<uint32> m_LevelOffsets;
    std::vector<uint32> m_Words;
    std::vector<float32> m_SlotFade;
    uint32 m_FloorLevel = 0;
    uint32 m_DirectoryOffset = 0;
    std::vector<Tier> m_Tiers;
    std::vector<Pool> m_Pools;
    std::vector<Waiting> m_Waiting;
    std::vector<PageStreaming::PageAddress> m_LateEntries;
};

} // namespace GameEngine::TerrainECS
