#pragma once

#include "TerrainECS/BakeWarningLatch.h"
#include "TerrainECS/TerrainColorMap.h"
#include "TerrainECS/TerrainModifierSystem.h"
#include "TerrainECS/TerrainService.h"

#include "Components/Terrain/TerrainModifierEffects.h"
#include "Spline/SplineData.h"
#include "Types/Types.h"

#include <deque>
#include <vector>

namespace GameEngine::TerrainECS
{

/// What a surface page bake reads (design page-streaming 3.8): the lattice, splat range, ground write
/// and modifier stacks of a tiled terrain. Every pointer of a stack points into this object, so a job
/// bakes from it while the scene changes. TerrainModifierSystem::SnapshotSurfaceBake makes it.
struct TerrainSurfaceBakeInputs
{
    TiledTerrainConfig Config;
    float32 WorldOriginX = 0.0f; // world-space corner of tile (0, 0)
    float32 WorldOriginZ = 0.0f;
    float32 SplatBakeMinH = 0.0f;
    float32 SplatBakeMaxH = 0.0f;
    bool SplatBakeRangeValid = false;
    TerrainColorGroundWrite GroundWrite;

    /// The height and splat stack, the grass stack and the color writers, each in its bake order.
    /// A stamp mask and a route are null: the surface bake reads neither.
    std::vector<ResolvedModifier> Splat;
    std::vector<ResolvedModifier> Grass;
    std::vector<ResolvedModifier> Color;

    /// The splat pass of this snapshot reports each distinct problem once.
    mutable BakeWarningLatch LayerClampWarnings;
    mutable BakeWarningLatch UnaddressableRulesWarnings;

    /// What the stacks point at. A deque: an append does not move what an earlier entry points at.
    std::deque<Spline::SplineData> Splines;
    std::deque<std::vector<Components::TerrainSurfaceRule>> RuleRows;
    std::deque<std::vector<uint8>> PaintMasks;
    std::deque<std::vector<float32>> ZoneColors;
};

} // namespace GameEngine::TerrainECS
