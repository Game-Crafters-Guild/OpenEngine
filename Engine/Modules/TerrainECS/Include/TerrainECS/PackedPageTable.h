#pragma once

#include "TerrainECS/PageTable.h"
#include "Types/Types.h"

#include <array>
#include <span>
#include <vector>

namespace GameEngine::TerrainECS
{

struct PageCacheGeometry;

/// The GPU words of one page table (PackPageTableWords's layout) at one version, and the word runs its
/// last versions changed. A copy of an older version (a published snapshot, a page-table ring slot)
/// catches up by writing only the runs changed since its version; past the history it writes every word.
class PackedPageTable
{
public:
    /// Brings the words to `table` as version `version`, greater than Version(): patches the runs the
    /// table changed in place, or packs every word after a reset or a change of size. Clears the
    /// table's changes.
    void Update(PageTable& table, const PageCacheGeometry& geometry, uint64 version);
    /// Makes this a copy of `source`, a later version of the same table: writes only the runs
    /// changed since this copy's version when `source`'s history reaches it, else every word.
    void CopyFrom(const PackedPageTable& source);
    /// Sets `out` to the runs of Words() changed after version `since`, sorted and disjoint. False when
    /// the history does not reach `since`: every word may differ.
    bool ChangedSince(uint64 since, std::vector<PageWordRange>& out) const;
    /// The 64-bit FNV-1a hash of the bytes of `words`. Two copies of a table with the same words have the
    /// same hash: a packed table, a whole pack and a ring slot's mapped words.
    static uint64 HashWords(std::span<const uint32> words);

    std::span<const uint32> Words() const { return m_Words; }
    /// 0 before the first Update.
    uint64 Version() const { return m_Version; }

private:
    /// The runs one version changed from the version before it.
    struct Record
    {
        uint64 Previous = 0;
        uint64 Version = 0;
        bool All = true; ///< every word changed
        std::vector<PageWordRange> Runs;
    };
    /// The versions a copy may lag and still catch up by runs: a ring slot is rewritten every few frames.
    static constexpr uint32 kRecords = 8u;

    /// Makes `version` the newest version; its runs are m_Runs unless `all`.
    void AddRecord(uint64 version, bool all);

    std::vector<uint32> m_Words;
    uint64 m_Version = 0;
    std::array<Record, kRecords> m_Records{};
    uint32 m_NewestRecord = 0;
    std::vector<PageWordRange> m_Runs; // scratch of Update and CopyFrom
};

} // namespace GameEngine::TerrainECS
