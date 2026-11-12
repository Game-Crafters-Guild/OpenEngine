#include "TerrainECS/PackedPageTable.h"

#include "TerrainECS/TerrainPages.h"
#include "CBTTerrain/CBTLayout.h"
#include "Core/CpuProfiler.h"
#include "Types/Fnv1a.h"

#include <algorithm>
#include <bit>
#include <cstring>

namespace GameEngine::TerrainECS
{

namespace
{

/// The runs one version records; a version that changes more counts as a change of every word.
constexpr std::size_t kMaxRecordRuns = 256u;

/// Sorts `runs` and joins the runs that overlap or touch.
void SortAndJoin(std::vector<PageWordRange>& runs)
{
    if (runs.size() < 2u)
        return;
    std::sort(runs.begin(), runs.end(),
              [](const PageWordRange& a, const PageWordRange& b) { return a.First < b.First; });
    std::size_t joined = 0;
    for (std::size_t next = 1; next < runs.size(); ++next)
    {
        PageWordRange& last = runs[joined];
        const uint32 lastEnd = last.First + last.Count;
        if (runs[next].First <= lastEnd)
        {
            last.Count = std::max(lastEnd, runs[next].First + runs[next].Count) - last.First;
            continue;
        }
        runs[++joined] = runs[next];
    }
    runs.resize(joined + 1u);
}

} // namespace

void PackedPageTable::Update(PageTable& table, const PageCacheGeometry& geometry, uint64 version)
{
    GE_CPU_PROFILE_SCOPE("Terrain.PageTable.Pack");
    const PageTableChanges& changes = table.Changes();
    const std::span<const float32> fades = table.SlotFades();
    const std::span<const uint32> words = table.Words();
    const std::size_t entriesAt = CBTTerrain::kCBTPageFadeOffsetWords + fades.size();
    bool all = changes.All || m_Words.size() != entriesAt + words.size();
    m_Runs.clear();
    if (all)
    {
        PackPageTableWords(table, geometry, m_Words);
    }
    else
    {
        if (changes.Fades)
        {
            uint32* fadeWords = m_Words.data() + CBTTerrain::kCBTPageFadeOffsetWords;
            for (std::size_t slot = 0; slot < fades.size(); ++slot)
                fadeWords[slot] = std::bit_cast<uint32>(fades[slot]);
            m_Runs.push_back(
                PageWordRange{CBTTerrain::kCBTPageFadeOffsetWords, static_cast<uint32>(fades.size())});
        }
        for (const PageWordRange& run : changes.Words)
        {
            std::memcpy(m_Words.data() + entriesAt + run.First, words.data() + run.First, run.Count * sizeof(uint32));
            m_Runs.push_back(PageWordRange{static_cast<uint32>(entriesAt) + run.First, run.Count});
        }
        SortAndJoin(m_Runs);
        all = m_Runs.size() > kMaxRecordRuns;
    }
    table.ClearChanges();
    AddRecord(version, all);
}

void PackedPageTable::CopyFrom(const PackedPageTable& source)
{
    if (this == &source)
        return;
    if (m_Words.size() == source.m_Words.size() && source.ChangedSince(m_Version, m_Runs))
    {
        for (const PageWordRange& run : m_Runs)
            std::memcpy(m_Words.data() + run.First, source.m_Words.data() + run.First, run.Count * sizeof(uint32));
    }
    else
    {
        m_Words.assign(source.m_Words.begin(), source.m_Words.end());
    }
    m_Version = source.m_Version;
    m_Records = source.m_Records;
    m_NewestRecord = source.m_NewestRecord;
}

bool PackedPageTable::ChangedSince(uint64 since, std::vector<PageWordRange>& out) const
{
    out.clear();
    uint64 at = m_Version;
    for (uint32 back = 0; back < kRecords && at != since; ++back)
    {
        const Record& record = m_Records[(m_NewestRecord + kRecords - back) % kRecords];
        if (record.Version != at || record.All)
            return false;
        out.insert(out.end(), record.Runs.begin(), record.Runs.end());
        at = record.Previous;
    }
    if (at != since)
        return false;
    SortAndJoin(out);
    return true;
}

uint64 PackedPageTable::HashWords(std::span<const uint32> words)
{
    return Hashing::Fnv1a64(words.data(), words.size_bytes());
}

void PackedPageTable::AddRecord(uint64 version, bool all)
{
    m_NewestRecord = (m_NewestRecord + 1u) % kRecords;
    Record& record = m_Records[m_NewestRecord];
    record.Previous = m_Version;
    record.Version = version;
    record.All = all;
    record.Runs.swap(m_Runs); // m_Runs takes the old record's storage
    m_Runs.clear();
    if (all)
        record.Runs.clear();
    m_Version = version;
}

} // namespace GameEngine::TerrainECS
