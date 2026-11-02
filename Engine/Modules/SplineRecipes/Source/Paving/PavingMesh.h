#pragma once

#include "Mathematics/Matrix4x4.h"
#include "Mathematics/Vector2.h"
#include "Mathematics/Vector3.h"
#include "SplineGeometry/AreaBoolean.h"
#include "SplineGeometry/SplineVertex.h"
#include "Types/Types.h"

#include <span>
#include <vector>

namespace GameEngine::SplineGeometry
{
struct AreaGround;
}

namespace GameEngine::SplineRecipes
{

struct PavingRoadUV;

// A paved region's lid, ready for ToEngineMesh.
struct PavingLidMesh
{
    std::vector<SplineGeometry::SplineVertex> Vertices;
    std::vector<uint32> Indices;
    Mathematics::Vector3 MinBounds{};
    Mathematics::Vector3 MaxBounds{};
};

// The phase, in meters, that `seed` moves the paving's pattern on the ground.
Mathematics::Vector2 PavingPatternPhase(uint32 seed);

// The lid of `ground` in the space of the owner at `ownerWorld`: triangles face up, normals average per point.
// Each of `roads` (RegionRoadUVs order) gives its strip's UV to its ribbon and one road width past it, outside
// the region's Area `outlines`. The rest takes U and V in world meters along the owner's x and z axes plus `phase`.
PavingLidMesh BuildPavingLidMesh(const SplineGeometry::AreaGround& ground, std::span<const PavingRoadUV> roads,
                                 std::span<const SplineGeometry::AreaRing> outlines,
                                 const Mathematics::Matrix4x4& ownerWorld, const Mathematics::Vector2& phase);

} // namespace GameEngine::SplineRecipes
