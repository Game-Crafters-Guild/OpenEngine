#pragma once

#include "Mathematics/Vector2.h"
#include "Mathematics/Vector3.h"
#include "SplineGeometry/AreaBoolean.h"
#include "Types/Types.h"

#include <span>
#include <vector>

namespace GameEngine::SplineRecipes
{

// A paved region's lid triangles after the cuts, with the mapping each takes. A mapping is an index the
// lid builder gives meaning to: a road, the lid's own mapping, or a seam band or course of a road.
struct LidPieces
{
    // The ground's lid points, then the cut points.
    std::vector<Mathematics::Vector3> Points;
    // Wound so cross(b - a, c - a) points up.
    std::vector<uint32> Triangles;
    std::vector<uint32> TriangleMapping;
};

// The mapping of a triangle that takes the lid's own world mapping.
inline constexpr uint32 kOwnMapping = ~0u;

// Gives the part of each triangle of mapping `claim` where `margin` (one value per point) is not
// negative to mapping `index`, cutting a crossed triangle along the zero. A cut point is shared by both
// sides of its edge; `margin` grows with the cut points, each at 0.
void CutAlongZero(LidPieces& pieces, std::vector<float32>& margin, uint32 claim, uint32 index);

// Meters from `xz` to the nearest edge of `outlines`: positive outside all of them, negative inside one,
// the largest float when there are none.
float32 OutsideOutlines(std::span<const SplineGeometry::AreaRing> outlines, const Mathematics::Vector2& xz);

} // namespace GameEngine::SplineRecipes
