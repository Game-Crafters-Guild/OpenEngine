#include "TerrainECS/PageTable.h"

#include "CBTTerrain/CBTLayout.h"
#include "PageStreaming/PageTableEntry.h"

#include <algorithm>
#include <cassert>

namespace GameEngine::TerrainECS
{

using PageStreaming::kNoPage;

namespace
{

/// The levels one block holds.
constexpr uint32 kPageTableBlockLevels = 6u;
/// The words of one tier's descriptor.
constexpr uint32 kPageTableTierWords = 4u;
/// A directory word with no block.
constexpr uint32 kNoPageBlock = 0xFFFFFFFFu;
/// The fraction of the budget a tiered table's dense top tier fits: a quarter.
constexpr uint64 kDenseTopTierBudgetFraction = 4u;

// The first word of level k below a block's root (k from 1): 4 + 16 + ... + 4^(k-1). At k = levels + 1
// it is the words of a block's entries, where the next tier's directory starts.
uint32 BlockLevelOffset(uint32 k)
{
    return ((1u << (2u * k)) - 4u) / 3u;
}

uint64 LevelEntries(const PageStreaming::PageStoreLevel& level)
{
    return static_cast<uint64>(level.PagesX) * level.PagesZ;
}

} // namespace

void PageTable::Reset(uint32 samplesX, uint32 samplesZ, uint32 faces, uint32 slotCount, const PageTableBudget& budget)
{
    assert((faces == 1u || faces == PageStreaming::kPageStoreMaxFaces) && "a field has one face or six");
    m_Levels = PageStreaming::BuildPageStoreLevels(samplesX, samplesZ);
    m_Faces = faces;
    m_SlotFade.assign(slotCount, 1.0f);
    m_LevelOffsets.assign(m_Levels.size(), 0u);
    m_Tiers.clear();
    m_Pools.clear();
    m_Waiting.clear();
    m_LateEntries.clear();
    m_FloorLevel = 0;
    m_DirectoryOffset = 0;
    m_Words.clear();
    if (m_Levels.empty())
        return;
    const uint64 entries = faces * (m_Levels.back().FirstEntry + LevelEntries(m_Levels.back()));
    if (CBTTerrain::kCBTPageFadeOffsetWords + slotCount + entries <= budget.DenseWords)
    {
        uint64 at = 0;
        for (std::size_t level = 0; level < m_Levels.size(); ++level)
        {
            m_LevelOffsets[level] = static_cast<uint32>(at);
            at += faces * LevelEntries(m_Levels[level]);
        }
        m_Words.assign(static_cast<std::size_t>(entries), kNoPage);
        return;
    }

    // The floor: the finest level whose entries over every face, with every coarser level's, fit a
    // quarter of the budget.
    const uint64 topBudget = budget.DenseWords / kDenseTopTierBudgetFraction;
    uint64 dense = faces * LevelEntries(m_Levels.back());
    m_FloorLevel = static_cast<uint32>(m_Levels.size()) - 1u;
    while (m_FloorLevel > 0 && dense + faces * LevelEntries(m_Levels[m_FloorLevel - 1u]) <= topBudget)
        dense += faces * LevelEntries(m_Levels[--m_FloorLevel]);

    for (uint32 root = m_FloorLevel; root > 0;)
    {
        const uint32 levels = std::min(kPageTableBlockLevels, root);
        m_Tiers.push_back(Tier{root, levels, BlockLevelOffset(levels + 1u), 0u, 0u});
        root -= levels;
    }
    uint32 at = static_cast<uint32>(m_Tiers.size()) * kPageTableTierWords;
    for (uint32 level = static_cast<uint32>(m_Levels.size()); level-- > m_FloorLevel;)
    {
        m_LevelOffsets[level] = at;
        at += static_cast<uint32>(faces * LevelEntries(m_Levels[level]));
    }
    m_DirectoryOffset = at;
    at += static_cast<uint32>(faces * LevelEntries(m_Levels[m_FloorLevel]));
    m_Pools.resize(m_Tiers.size());
    for (std::size_t tier = 0; tier < m_Tiers.size(); ++tier)
    {
        Tier& shape = m_Tiers[tier];
        const bool last = tier + 1u == m_Tiers.size();
        if (!last)
            shape.BlockWords += 1u << (2u * shape.Levels);
        // A block holds at least one resident page, so a pool never needs more blocks than slots.
        shape.Blocks = std::max(1u, std::min(last ? budget.LeafBlocks : budget.MidBlocks, slotCount));
        shape.PoolOffset = at;
        at += shape.Blocks * shape.BlockWords;
        // A block level's offset is inside its tier's block: k levels below the tier's root.
        for (uint32 k = 1; k <= shape.Levels; ++k)
            m_LevelOffsets[shape.RootLevel - k] = BlockLevelOffset(k);
        Pool& pool = m_Pools[tier];
        pool.Free.resize(shape.Blocks);
        for (uint32 block = 0; block < shape.Blocks; ++block)
            pool.Free[block] = shape.Blocks - 1u - block; // the lowest index is taken first
        pool.Residents.assign(shape.Blocks, 0u);
        pool.Owner.assign(shape.Blocks, 0u);
        pool.Parent.assign(shape.Blocks, kNoPageBlock);
        pool.RootX.assign(shape.Blocks, 0u);
        pool.RootZ.assign(shape.Blocks, 0u);
        pool.RootFace.assign(shape.Blocks, 0u);
    }
    m_Words.assign(at, kNoPage); // kNoPage is also kNoPageBlock: every directory word starts empty
    for (std::size_t tier = 0; tier < m_Tiers.size(); ++tier)
    {
        uint32* descriptor = m_Words.data() + tier * kPageTableTierWords;
        descriptor[0] = m_Tiers[tier].RootLevel;
        descriptor[1] = m_Tiers[tier].Levels;
        descriptor[2] = m_Tiers[tier].BlockWords;
        descriptor[3] = m_Tiers[tier].PoolOffset;
    }
}

uint32 PageTable::DenseWord(const PageStreaming::PageAddress& page) const
{
    const PageStreaming::PageStoreLevel& shape = m_Levels[page.Level];
    return m_LevelOffsets[page.Level] + (page.Face * shape.PagesZ + page.Z) * shape.PagesX + page.X;
}

uint32 PageTable::FloorDirectoryWord(const PageStreaming::PageAddress& page) const
{
    const uint32 shift = m_FloorLevel - page.Level;
    const PageStreaming::PageStoreLevel& floor = m_Levels[m_FloorLevel];
    return m_DirectoryOffset + (page.Face * floor.PagesZ + (page.Z >> shift)) * floor.PagesX + (page.X >> shift);
}

uint32 PageTable::BlockWord(const Tier& tier, uint32 block, uint32 rootX, uint32 rootZ, uint32 level,
                            uint32 pageX, uint32 pageZ) const
{
    const uint32 base = tier.PoolOffset + block * tier.BlockWords;
    const uint32 deepest = tier.RootLevel - tier.Levels;
    if (level >= deepest)
    {
        const uint32 k = tier.RootLevel - level;
        return base + BlockLevelOffset(k) + (pageZ - (rootZ << k)) * (1u << k) + (pageX - (rootX << k));
    }
    // The next tier's directory: one word per page of this block's deepest level.
    const uint32 below = deepest - level;
    return base + BlockLevelOffset(tier.Levels + 1u) + ((pageZ >> below) - (rootZ << tier.Levels)) * (1u << tier.Levels) +
           ((pageX >> below) - (rootX << tier.Levels));
}

uint32 PageTable::Entry(const PageStreaming::PageAddress& page) const
{
    if (page.Level >= m_FloorLevel)
        return m_Words[DenseWord(page)];
    const uint32 level = page.Level;
    const uint32 pageX = page.X;
    const uint32 pageZ = page.Z;
    const uint32 shift = m_FloorLevel - level;
    uint32 rootX = pageX >> shift;
    uint32 rootZ = pageZ >> shift;
    uint32 block = m_Words[FloorDirectoryWord(page)];
    for (const Tier& tier : m_Tiers)
    {
        if (block == kNoPageBlock)
            return kNoPage; // no page under this root is resident
        const uint32 word = m_Words[BlockWord(tier, block, rootX, rootZ, level, pageX, pageZ)];
        const uint32 deepest = tier.RootLevel - tier.Levels;
        if (level >= deepest)
            return word;
        block = word;
        rootX = pageX >> (deepest - level);
        rootZ = pageZ >> (deepest - level);
    }
    return kNoPage;
}

bool PageTable::Insert(const PageStreaming::PageAddress& page, uint32 entry)
{
    const uint32 level = page.Level;
    const uint32 pageX = page.X;
    const uint32 pageZ = page.Z;
    const uint32 shift = m_FloorLevel - level;
    uint32 rootX = pageX >> shift;
    uint32 rootZ = pageZ >> shift;
    uint32 owner = FloorDirectoryWord(page);
    uint32 parent = kNoPageBlock;
    for (uint32 tierIndex = 0; tierIndex < m_Tiers.size(); ++tierIndex)
    {
        const Tier& tier = m_Tiers[tierIndex];
        Pool& pool = m_Pools[tierIndex];
        uint32 block = m_Words[owner];
        if (block == kNoPageBlock)
        {
            if (pool.Free.empty())
            {
                // A block taken above on this walk holds nothing yet: give it back.
                if (parent != kNoPageBlock && m_Pools[tierIndex - 1u].Residents[parent] == 0)
                    Release(tierIndex - 1u, parent);
                return false;
            }
            block = pool.Free.back();
            pool.Free.pop_back();
            std::fill_n(m_Words.begin() + tier.PoolOffset + block * tier.BlockWords, tier.BlockWords, kNoPage);
            pool.Residents[block] = 0;
            pool.Owner[block] = owner;
            pool.Parent[block] = parent;
            pool.RootX[block] = rootX;
            pool.RootZ[block] = rootZ;
            pool.RootFace[block] = page.Face;
            m_Words[owner] = block;
            if (parent != kNoPageBlock)
                ++m_Pools[tierIndex - 1u].Residents[parent];
        }
        const uint32 at = BlockWord(tier, block, rootX, rootZ, level, pageX, pageZ);
        const uint32 deepest = tier.RootLevel - tier.Levels;
        if (level >= deepest)
        {
            if (m_Words[at] == kNoPage)
                ++pool.Residents[block];
            m_Words[at] = entry;
            return true;
        }
        owner = at;
        parent = block;
        rootX = pageX >> (deepest - level);
        rootZ = pageZ >> (deepest - level);
    }
    return false;
}

void PageTable::Remove(const PageStreaming::PageAddress& page)
{
    const uint32 level = page.Level;
    const uint32 pageX = page.X;
    const uint32 pageZ = page.Z;
    const uint32 shift = m_FloorLevel - level;
    uint32 rootX = pageX >> shift;
    uint32 rootZ = pageZ >> shift;
    uint32 block = m_Words[FloorDirectoryWord(page)];
    for (uint32 tierIndex = 0; tierIndex < m_Tiers.size(); ++tierIndex)
    {
        if (block == kNoPageBlock)
            return;
        const Tier& tier = m_Tiers[tierIndex];
        const uint32 at = BlockWord(tier, block, rootX, rootZ, level, pageX, pageZ);
        const uint32 deepest = tier.RootLevel - tier.Levels;
        if (level < deepest)
        {
            block = m_Words[at];
            rootX = pageX >> (deepest - level);
            rootZ = pageZ >> (deepest - level);
            continue;
        }
        if (m_Words[at] == kNoPage)
            return;
        m_Words[at] = kNoPage;
        if (--m_Pools[tierIndex].Residents[block] == 0)
        {
            Release(tierIndex, block);
            RetryWaiting();
        }
        return;
    }
}

void PageTable::Release(uint32 tier, uint32 block)
{
    // Every word of the freed block already reads kNoPage: each Remove clears its own entry and each
    // child block's Release its directory word. Insert fills the block again when it takes it.
    Pool& pool = m_Pools[tier];
    m_Words[pool.Owner[block]] = kNoPageBlock;
    pool.Free.push_back(block);
    const uint32 parent = pool.Parent[block];
    if (parent != kNoPageBlock && --m_Pools[tier - 1u].Residents[parent] == 0)
        Release(tier - 1u, parent);
}

void PageTable::RetryWaiting()
{
    for (std::size_t i = 0; i < m_Waiting.size();)
    {
        const Waiting waiting = m_Waiting[i];
        if (!Insert(waiting.Address, waiting.Entry))
        {
            ++i;
            continue;
        }
        m_LateEntries.push_back(waiting.Address);
        m_Waiting.erase(m_Waiting.begin() + static_cast<std::ptrdiff_t>(i));
    }
}

void PageTable::SetEntry(const PageStreaming::PageAddress& page, uint32 entry)
{
    assert(page.Face < m_Faces && "the page's face is in the table");
    if (page.Level >= m_FloorLevel)
    {
        m_Words[DenseWord(page)] = entry;
        return;
    }
    std::erase_if(m_Waiting, [&page](const Waiting& waiting) { return waiting.Address == page; });
    if (entry == kNoPage)
    {
        Remove(page);
        return;
    }
    if (!Insert(page, entry))
        m_Waiting.push_back(Waiting{page, entry});
}

void PageTable::AppendResident(std::vector<PageStreaming::PageAddress>& out) const
{
    for (uint32 level = m_FloorLevel; level < m_Levels.size(); ++level)
        for (uint32 face = 0; face < m_Faces; ++face)
            for (uint32 z = 0; z < m_Levels[level].PagesZ; ++z)
                for (uint32 x = 0; x < m_Levels[level].PagesX; ++x)
                {
                    const PageStreaming::PageAddress page{static_cast<uint8>(face), static_cast<uint8>(level), x, z};
                    if (m_Words[DenseWord(page)] != kNoPage)
                        out.push_back(page);
                }
    // Below the floor, entries live only in blocks, and a block in use holds at least one.
    for (std::size_t tierIndex = 0; tierIndex < m_Tiers.size(); ++tierIndex)
    {
        const Tier& tier = m_Tiers[tierIndex];
        const Pool& pool = m_Pools[tierIndex];
        for (uint32 block = 0; block < tier.Blocks; ++block)
        {
            if (pool.Residents[block] == 0)
                continue;
            for (uint32 k = 1; k <= tier.Levels; ++k)
            {
                const uint32 level = tier.RootLevel - k;
                const uint32 side = 1u << k;
                const uint32* words = m_Words.data() + tier.PoolOffset + block * tier.BlockWords + BlockLevelOffset(k);
                for (uint32 local = 0; local < side * side; ++local)
                    if (words[local] != kNoPage)
                        out.push_back(PageStreaming::PageAddress{pool.RootFace[block], static_cast<uint8>(level),
                                                                 (pool.RootX[block] << k) + local % side,
                                                                 (pool.RootZ[block] << k) + local / side});
            }
        }
    }
}

void PageTable::TakeLateEntries(std::vector<PageStreaming::PageAddress>& out)
{
    out.insert(out.end(), m_LateEntries.begin(), m_LateEntries.end());
    m_LateEntries.clear();
}

} // namespace GameEngine::TerrainECS
