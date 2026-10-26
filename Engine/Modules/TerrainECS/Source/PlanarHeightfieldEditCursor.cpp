#include "TerrainECS/PlanarHeightfieldEditCursor.h"

#include "TerrainECS/TerrainService.h"

namespace GameEngine::TerrainECS
{

namespace
{

// The rectangle an edit over lattice samples [minX, maxX) x [minZ, maxZ) of `query` changes: every
// quad that touches one of its samples.
Mathematics::Rect RectOfSamples(const SurfaceHeightQuery& query, int64 minX, int64 minZ, int64 maxX, int64 maxZ)
{
    const float32 spacingX = query.LatticeSpacingX();
    const float32 spacingZ = query.LatticeSpacingZ();
    return Mathematics::Rect::FromEdges(query.FootprintOriginX() + static_cast<float32>(minX - 1) * spacingX,
                                        query.FootprintOriginZ() + static_cast<float32>(minZ - 1) * spacingZ,
                                        query.FootprintOriginX() + static_cast<float32>(maxX) * spacingX,
                                        query.FootprintOriginZ() + static_cast<float32>(maxZ) * spacingZ);
}

Mathematics::Rect WholeFootprint(const SurfaceHeightQuery& query)
{
    return Mathematics::Rect::FromEdges(query.FootprintOriginX(), query.FootprintOriginZ(),
                                        query.FootprintOriginX() + query.FootprintSizeX(),
                                        query.FootprintOriginZ() + query.FootprintSizeZ());
}

} // namespace

void PlanarHeightfieldEditCursor::Reset(const SurfaceHeightQuery& query)
{
    m_WholeMap = query.WholeMapIdentity();
    m_Version = query.WholeMap() ? query.WholeMap()->HeightfieldVersion : query.Edits().OverlayVersion;
    m_Source = query.Edits().Source;
}

void PlanarHeightfieldEditCursor::CollectEdits(const SurfaceHeightQuery& query, std::vector<Mathematics::Rect>& out)
{
    if (query.WholeMapIdentity() != m_WholeMap)
    {
        Reset(query);
        return;
    }
    if (const TerrainData* wholeMap = query.WholeMap())
    {
        DirtyRegionLog::Region region;
        if (wholeMap->HeightfieldVersion != m_Version && wholeMap->HeightfieldDirtyLog.CollectSince(m_Version, region))
            out.push_back(RectOfSamples(query, region.MinX, region.MinZ, region.MaxX, region.MaxZ));
        m_Version = wholeMap->HeightfieldVersion;
        return;
    }
    const SurfaceHeightEdits& edits = query.Edits();
    if (edits.Source != m_Source)
    {
        // A first source is the terrain's pages arriving: a reader waited for them, it saw no height.
        if (m_Source != 0 && edits.Source != 0)
            out.push_back(WholeFootprint(query));
        Reset(query);
        return;
    }
    if (edits.OverlayVersion == m_Version)
        return;
    if (edits.OverlayChangedSince == m_Version)
        out.push_back(RectOfSamples(query, edits.OverlayChanged.MinX, edits.OverlayChanged.MinZ,
                                    static_cast<int64>(edits.OverlayChanged.MaxX) + 1,
                                    static_cast<int64>(edits.OverlayChanged.MaxZ) + 1));
    else
        out.push_back(WholeFootprint(query));
    m_Version = edits.OverlayVersion;
}

} // namespace GameEngine::TerrainECS
