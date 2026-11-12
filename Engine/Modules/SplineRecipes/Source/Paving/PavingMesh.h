#pragma once

#include "Mathematics/Matrix4x4.h"
#include "Mathematics/Vector2.h"
#include "Mathematics/Vector3.h"
#include "Mathematics/Vector4.h"
#include "SplineGeometry/AreaBoolean.h"
#include "SplineGeometry/SplineVertex.h"
#include "Types/Types.h"

#include <optional>
#include <span>
#include <vector>

namespace GameEngine::SplineGeometry
{
struct AreaGround;
}

namespace GameEngine::SplineRecipes
{

struct PavingRoadSeam;
struct PavingRoadUV;
struct PavingSeamMaterial;

// A part of a paved region's lid, ready for ToEngineMesh.
struct PavingLidMesh
{
    std::vector<SplineGeometry::SplineVertex> Vertices;
    std::vector<uint32> Indices;
    // One per vertex in a part that reads them, else empty: the crossfade's second UV, and the tint
    // whose alpha weighs it.
    std::vector<Mathematics::Vector2> SecondUV;
    std::vector<Mathematics::Vector4> Colors;
    Mathematics::Vector3 MinBounds{};
    Mathematics::Vector3 MaxBounds{};
};

// A seam whose Blend draws as a hard cut (PavingSeams::DrawsHardCut): the yielding road's entity id and
// what it yields to there.
struct PavingHardCutSeam
{
    uint32 Road = 0;
    // The entity id of the road it yields to; empty for the lid's own field across an Area outline.
    std::optional<uint32> PartnerRoad;
};

// A paved region's lid in its draws: the paving, the crossfade bands, and each road's course.
struct PavingLidParts
{
    PavingLidMesh Lid;
    PavingLidMesh Band;
    // One per road of the region, empty where the road draws no course.
    std::vector<PavingLidMesh> Courses;
    std::vector<PavingHardCutSeam> HardCutSeams;
};

// The phase, in meters, that `seed` moves the paving's pattern on the ground.
Mathematics::Vector2 PavingPatternPhase(uint32 seed);

// The lid of `ground` in the space of the owner at `ownerWorld`: triangles face up, normals average per point.
// Each of `roads` (RegionRoadUVs order) gives its strip's UV to its ribbon and one road width past it, outside
// the region's Area `outlines`. The rest takes U and V in world meters along the owner's x and z axes plus `phase`.
// Each road meets the fields it yields to with the look of `seams` (one per road), read with `material`.
PavingLidParts BuildPavingLidMesh(const SplineGeometry::AreaGround& ground, std::span<const PavingRoadUV> roads,
                                  std::span<const PavingRoadSeam> seams,
                                  std::span<const SplineGeometry::AreaRing> outlines,
                                  const Mathematics::Matrix4x4& ownerWorld, const Mathematics::Vector2& phase,
                                  const PavingSeamMaterial& material);

} // namespace GameEngine::SplineRecipes
