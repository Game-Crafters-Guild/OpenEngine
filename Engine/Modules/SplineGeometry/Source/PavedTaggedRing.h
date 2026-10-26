#pragma once

#include "Mathematics/Rect.h"
#include "Mathematics/Vector2.h"
#include "SplineGeometry/PavedFootprint.h"
#include "Types/Types.h"

#include <span>
#include <vector>

namespace GameEngine::SplineGeometry
{

class PavedArmPath;

// A ring of a paving's input with the source of each edge (Edges[i] from Ring[i] to Ring[i + 1],
// the last closing it). A sliver's ring is empty.
struct PavedTaggedRing
{
    AreaRing Ring;
    std::vector<PavedEdge> Edges;
    // The ring's box grown by kPavedTouchMeters (TouchBoundsOf).
    Mathematics::Rect Bounds;
};

// The box around `points`, grown by kPavedTouchMeters so that touching boxes overlap.
Mathematics::Rect TouchBoundsOf(std::span<const Mathematics::Vector2> points);

// `ring` with every edge from `source` `index`.
PavedTaggedRing TagRing(const AreaRing& ring, PavedEdgeSource source, uint32 index);

// An arm's ribbon trimmed `stop` meters from the node: up its right side, across its far cap, back
// down its left side, and across the node's cap; counter-clockwise from above. The far cap is Open
// when the road goes on past the stop, else ArmEnd; the node's cap is ArmEnd.
PavedTaggedRing BuildRibbon(const PavedArmPath& path, float32 stop, uint32 arm);

// Whether segment ab overlaps `ring`'s area or comes within kPavedTouchMeters of its outline.
bool SegmentMeetsRing(const Mathematics::Vector2& a, const Mathematics::Vector2& b, const PavedTaggedRing& ring);

// Whether two rings overlap or come within kPavedTouchMeters of each other.
bool RingsMeet(const PavedTaggedRing& first, const PavedTaggedRing& second);

} // namespace GameEngine::SplineGeometry
