#include "SplineRecipes/ConformGroundWatch.h"

#include "Components/Terrain/Terrain.h"
#include "Components/Transform.h"
#include "ECS/Query.h"
#include "ECS/World.h"
#include "Mathematics/Rect.h"
#include "SplineRecipes/SplineSurfaceConform.h"
#include "SplineSurfaceConformInternal.h"
#include "TerrainECS/PlanarHeightfieldEditCursor.h"
#include "TerrainECS/SurfaceHeightQuery.h"
#include "TerrainECS/TerrainService.h"

#include <algorithm>

namespace GameEngine::SplineRecipes
{

struct ConformGroundWatch::TerrainCursor
{
    uint32 EntityId = 0;
    TerrainECS::PlanarHeightfieldEditCursor Edits;
    TerrainECS::SurfaceHeightPageStamp Pages;
    bool Seen = false;
};

ConformGroundWatch::ConformGroundWatch() = default;
ConformGroundWatch::~ConformGroundWatch() = default;
ConformGroundWatch::ConformGroundWatch(ConformGroundWatch&&) noexcept = default;
ConformGroundWatch& ConformGroundWatch::operator=(ConformGroundWatch&&) noexcept = default;

uint64 ConformGroundWatch::Revision(ECS::World& world, const TerrainECS::TerrainModifierSystem* modifierSystem,
                                    const Mathematics::AABB& reach, uint64 groundStamp)
{
    if (m_Folds > 0 && groundStamp == m_Stamp && reach == m_Reach)
        return m_Revision;
    const uint64 terrainSet = ConformTerrainSetRevision(world, modifierSystem, reach);
    const bool stepped = ReadTerrainSteps(world, reach);
    if (m_Folds == 0 || terrainSet != m_TerrainSet || stepped || reach != m_Reach)
        ++m_Revision;
    m_TerrainSet = terrainSet;
    m_Stamp = groundStamp;
    m_Reach = reach;
    ++m_Folds;
    return m_Revision;
}

void ConformGroundWatch::RecordBuild(const ConformMissTally& tally)
{
    m_FollowPages = tally.Misses > 0u || tally.CoarseHeights > 0u;
}

bool ConformGroundWatch::ReadTerrainSteps(ECS::World& world, const Mathematics::AABB& reach)
{
    auto* terrainService = TerrainECS::TerrainService::TryGet();
    if (!terrainService)
    {
        m_Terrains.clear();
        return false;
    }
    for (TerrainCursor& cursor : m_Terrains)
        cursor.Seen = false;

    const Mathematics::Rect reachXZ = Mathematics::Rect::FromEdges(reach.min.x, reach.min.z, reach.max.x, reach.max.z);
    bool stepped = false;
    world.Query<ECS::Read<Components::Terrain>, ECS::Read<Components::WorldTransform>>().Each(
        [&](ECS::EntityHandle entity, const Components::Terrain& terrain, const Components::WorldTransform& worldXf)
        {
            if (terrain.Domain == Components::TerrainDomain::Planar)
                stepped |= ReadTerrainStep(terrainService->SurfaceQuery(terrain, worldXf), entity.id, reachXZ);
        });
    std::erase_if(m_Terrains, [](const TerrainCursor& cursor) { return !cursor.Seen; });
    return stepped;
}

bool ConformGroundWatch::ReadTerrainStep(const TerrainECS::SurfaceHeightQuery& query, uint32 entityId,
                                         const Mathematics::Rect& reachXZ)
{
    if (!query.IsValid())
        return false;
    const auto found = std::find_if(m_Terrains.begin(), m_Terrains.end(),
                                    [entityId](const TerrainCursor& cursor) { return cursor.EntityId == entityId; });
    if (found == m_Terrains.end())
    {
        // ConformTerrainSetRevision reports a new terrain that meets the reach.
        TerrainCursor& cursor = m_Terrains.emplace_back();
        cursor.EntityId = entityId;
        cursor.Edits.Reset(query);
        cursor.Pages = query.PageStamp();
        cursor.Seen = true;
        return false;
    }
    found->Seen = true;
    m_Changed.clear();
    found->Edits.CollectEdits(query, m_Changed);
    // A record that does not reach back to the stamp may hold any change of the terrain.
    if (m_FollowPages && !query.CollectPageChanges(found->Pages, m_Changed))
        m_Changed.push_back(query.Footprint());
    found->Pages = query.PageStamp();
    return std::any_of(m_Changed.begin(), m_Changed.end(),
                       [&reachXZ](const Mathematics::Rect& rect) { return rect.Overlaps(reachXZ); });
}

} // namespace GameEngine::SplineRecipes
