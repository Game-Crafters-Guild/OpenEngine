#pragma once

#include "Mathematics/Vector2.h"
#include "SplineGeometry/AreaBoolean.h"

#include <cstddef>
#include <span>
#include <vector>

namespace GameEngine::MarkupECS
{

struct MarkupFootprint;

// The corners of the ring that stands for a circle footprint (a sphere's) in the area.
inline constexpr std::size_t kFootprintCircleCorners = 32;

// A region's area as its display draws it: (the base ring ∪ the includes) − the excludes, the
// set MarkupRegionArea::Contains tests, a circle footprint taken as its kFootprintCircleCorners-gon
// inscribed. The footprints as rings through SplineGeometry::CombineAreas: its pieces, in no
// particular order, exact to a millimeter.
std::vector<SplineGeometry::AreaPiece> CombineRegionArea(std::span<const Mathematics::Vector2> base,
                                                         std::span<const MarkupFootprint> includes,
                                                         std::span<const MarkupFootprint> excludes);

} // namespace GameEngine::MarkupECS
