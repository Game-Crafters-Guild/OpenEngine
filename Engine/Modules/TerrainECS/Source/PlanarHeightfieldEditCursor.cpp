#include "TerrainECS/PlanarHeightfieldEditCursor.h"

#include "TerrainECS/TerrainService.h"

namespace GameEngine::TerrainECS
{

namespace
{

const void* TerrainOf(const PlanarHeightQuery& query)
{
    return query.Single ? static_cast<const void*>(query.Single) : static_cast<const void*>(query.Tiled);
}

uint64 PackCoordinate(const TileCoord& coord)
{
    return (static_cast<uint64>(static_cast<uint32>(coord.X)) << 32) | static_cast<uint32>(coord.Z);
}

// The rectangle an edit over samples [region.Min, region.Max) of a lattice from (originX, originZ)
// changes: every quad that touches one of its samples.
Mathematics::Rect RectOfSamples(const DirtyRegionLog::Region& region, float32 originX, float32 originZ,
                                float32 spacingX, float32 spacingZ)
{
    return Mathematics::Rect::FromEdges(originX + static_cast<float32>(region.MinX - 1) * spacingX,
                                        originZ + static_cast<float32>(region.MinZ - 1) * spacingZ,
                                        originX + static_cast<float32>(region.MaxX) * spacingX,
                                        originZ + static_cast<float32>(region.MaxZ) * spacingZ);
}

} // namespace

void PlanarHeightfieldEditCursor::Reset(const PlanarHeightQuery& query)
{
    m_Terrain = TerrainOf(query);
    m_Version = 0;
    m_TileVersions.clear();
    if (query.Single)
        m_Version = query.Single->HeightfieldVersion;
    else if (query.Tiled)
    {
        m_Version = query.Tiled->Revision;
        for (const auto& [coord, tile] : query.Tiled->Tiles)
            m_TileVersions[PackCoordinate(coord)] = tile->HeightfieldVersion;
    }
}

void PlanarHeightfieldEditCursor::CollectEdits(const PlanarHeightQuery& query, std::vector<Mathematics::Rect>& out)
{
    if (TerrainOf(query) != m_Terrain)
    {
        Reset(query);
        return;
    }
    const float32 spacingX = query.LatticeSpacingX();
    const float32 spacingZ = query.LatticeSpacingZ();
    DirtyRegionLog::Region region;
    if (query.Single && query.Single->HeightfieldVersion != m_Version)
    {
        if (query.Single->HeightfieldDirtyLog.CollectSince(m_Version, region))
            out.push_back(RectOfSamples(region, query.LatticeOriginX(), query.LatticeOriginZ(), spacingX, spacingZ));
        m_Version = query.Single->HeightfieldVersion;
    }
    else if (query.Tiled && query.Tiled->Revision != m_Version)
    {
        const float32 tileSize = query.Tiled->Config.TileWorldSize;
        for (const auto& [coord, tile] : query.Tiled->Tiles)
        {
            const auto [seen, inserted] = m_TileVersions.try_emplace(PackCoordinate(coord), tile->HeightfieldVersion);
            if (inserted || seen->second == tile->HeightfieldVersion)
                continue;
            if (tile->HeightfieldDirtyLog.CollectSince(seen->second, region))
                out.push_back(RectOfSamples(region, query.Tiled->WorldOriginX + static_cast<float32>(coord.X) * tileSize,
                                            query.Tiled->WorldOriginZ + static_cast<float32>(coord.Z) * tileSize,
                                            spacingX, spacingZ));
            seen->second = tile->HeightfieldVersion;
        }
        if (m_TileVersions.size() > query.Tiled->Tiles.size())
            std::erase_if(m_TileVersions, [&](const auto& entry) {
                const TileCoord coord{static_cast<int32>(entry.first >> 32), static_cast<int32>(entry.first & 0xFFFFFFFFu)};
                return !query.Tiled->Tiles.contains(coord);
            });
        m_Version = query.Tiled->Revision;
    }
}

} // namespace GameEngine::TerrainECS
