#pragma once

#include "Mathematics/Geometry.h"
#include "Mathematics/Rect.h"
#include "Types/Types.h"

#include <vector>

namespace GameEngine::ECS { class World; }
namespace GameEngine::TerrainECS { class SurfaceHeightQuery; class TerrainModifierSystem; }

namespace GameEngine::SplineRecipes
{

struct ConformMissTally;

// The ground revision of one reader's reach: it changes when the terrain set in the reach changes, when an overlay
// step meets the reach and, after a build that read a coarse page or missed, when a page in the reach changes.
// Revision reads the ground again on its first call and when the ground stamp or the reach changes. Main thread only.
class ConformGroundWatch
{
public:
    ConformGroundWatch();
    ~ConformGroundWatch();
    ConformGroundWatch(ConformGroundWatch&&) noexcept;
    ConformGroundWatch& operator=(ConformGroundWatch&&) noexcept;

    [[nodiscard]] uint64 Revision(ECS::World& world, const TerrainECS::TerrainModifierSystem* modifierSystem,
                                  const Mathematics::AABB& reach, uint64 groundStamp);

    // Records what the build that landed read. Call Revision again before the build is marked built, so the
    // pages the build fetched do not move it.
    void RecordBuild(const ConformMissTally& tally);

    // Test seam: the times Revision read the ground again.
    uint64 GetFoldCountForTests() const { return m_Folds; }

private:
    // One planar terrain's place in its overlay steps and page changes.
    struct TerrainCursor;

    // Moves every planar terrain's cursor to now: true when a step it passed meets `reach`.
    bool ReadTerrainSteps(ECS::World& world, const Mathematics::AABB& reach);
    bool ReadTerrainStep(const TerrainECS::SurfaceHeightQuery& query, uint32 entityId,
                         const Mathematics::Rect& reachXZ);

    std::vector<TerrainCursor> m_Terrains;
    std::vector<Mathematics::Rect> m_Changed; // scratch of ReadTerrainStep
    Mathematics::AABB m_Reach = Mathematics::AABB::Empty();
    uint64 m_Stamp = 0;
    uint64 m_TerrainSet = 0;
    uint64 m_Revision = 0;
    uint64 m_Folds = 0;
    // The last build read a coarse page or missed: a page change in the reach can move its ground.
    bool m_FollowPages = true;
};

} // namespace GameEngine::SplineRecipes
