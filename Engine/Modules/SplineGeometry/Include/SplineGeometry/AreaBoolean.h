#pragma once

#include "Mathematics/Vector2.h"
#include "Types/Types.h"

#include <span>
#include <vector>

namespace GameEngine::SplineGeometry
{

// A closed ring on the ground plane (x, z), in meters, its closing point not repeated. An outer
// ring runs counter-clockwise seen from above (positive Mathematics::PolygonDoubledSignedArea),
// a hole clockwise.
using AreaRing = std::vector<Mathematics::Vector2>;

// One connected piece of an area: its outer ring and the holes inside it. No two rings cross.
struct AreaPiece
{
    AreaRing Outer;
    std::vector<AreaRing> Holes;
};

// The union of `unions` minus the union of `cuts`, each ring taken whichever way it is wound.
// The pieces in no particular order; an island inside a hole is a piece of its own. Empty when
// the cuts cover everything. Exact to a millimeter. With CombineTouchingAreas, the engine's polygon boolean.
std::vector<AreaPiece> CombineAreas(std::span<const AreaRing> unions, std::span<const AreaRing> cuts);

// CombineAreas with what touches welded: rings closer than `touchMeters` join with no gap between
// them, a hole of their union narrower than `sliverMeters` on average (twice its area over its
// perimeter: the lens left between two sampled curves that touch) is filled, and a strip of the
// result narrower than `touchMeters` (between a cut and the area's edge, or between two cuts) is
// removed. Elsewhere the boundary is the inputs' to a hundredth of a millimeter. Costs a union and
// four offsets more than CombineAreas.
std::vector<AreaPiece> CombineTouchingAreas(std::span<const AreaRing> unions, std::span<const AreaRing> cuts,
                                            float32 touchMeters, float32 sliverMeters);

} // namespace GameEngine::SplineGeometry
