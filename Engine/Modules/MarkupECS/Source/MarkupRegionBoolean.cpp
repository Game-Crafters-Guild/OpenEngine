#include "MarkupECS/MarkupRegionBoolean.h"

#include "MarkupECS/MarkupRegionArea.h"
#include "Mathematics/Geometry.h"
#include "Mathematics/VectorOps.h"

#include <cmath>

namespace GameEngine::MarkupECS
{

using Mathematics::Vector2;
using SplineGeometry::AreaRing;

namespace
{

AreaRing FootprintRing(const MarkupFootprint& footprint)
{
    if (!footprint.IsCircle())
        return footprint.Ring;
    AreaRing ring;
    ring.reserve(kFootprintCircleCorners);
    for (std::size_t corner = 0; corner < kFootprintCircleCorners; ++corner)
    {
        const float angle = 2.0f * Mathematics::Pi * static_cast<float>(corner) / static_cast<float>(kFootprintCircleCorners);
        ring.emplace_back(footprint.Center.x + footprint.Radius * std::cos(angle),
                          footprint.Center.y + footprint.Radius * std::sin(angle));
    }
    return ring;
}

// `footprints` as rings, after `rings`.
std::vector<AreaRing> AppendFootprintRings(std::vector<AreaRing> rings, std::span<const MarkupFootprint> footprints)
{
    rings.reserve(rings.size() + footprints.size());
    for (const MarkupFootprint& footprint : footprints)
        rings.push_back(FootprintRing(footprint));
    return rings;
}

} // namespace

std::vector<SplineGeometry::AreaPiece> CombineRegionArea(std::span<const Vector2> base,
                                                         std::span<const MarkupFootprint> includes,
                                                         std::span<const MarkupFootprint> excludes)
{
    const std::vector<AreaRing> unions = AppendFootprintRings({AreaRing(base.begin(), base.end())}, includes);
    return SplineGeometry::CombineAreas(unions, AppendFootprintRings({}, excludes));
}

} // namespace GameEngine::MarkupECS
