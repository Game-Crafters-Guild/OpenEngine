#include "PavedTaggedRing.h"

#include "PavedArmPath.h"

#include "Mathematics/Geometry.h"

#include <algorithm>
#include <array>

namespace GameEngine::SplineGeometry
{

using Mathematics::DistanceToSegment;
using Mathematics::Rect;
using Mathematics::Vector2;

namespace
{

// The distance between segments ab and cd: 0 when they cross, else the nearest end's.
float32 SegmentDistance(const Vector2& a, const Vector2& b, const Vector2& c, const Vector2& d)
{
    if (Mathematics::SegmentsIntersect(a, b, c, d))
        return 0.0f;
    return std::min({DistanceToSegment(a, c, d), DistanceToSegment(b, c, d), DistanceToSegment(c, a, b),
                     DistanceToSegment(d, a, b)});
}

} // namespace

Rect TouchBoundsOf(std::span<const Vector2> points)
{
    if (points.empty())
        return Rect{};
    Vector2 min = points.front();
    Vector2 max = min;
    for (const Vector2& point : points)
    {
        min = Vector2(std::min(min.x, point.x), std::min(min.y, point.y));
        max = Vector2(std::max(max.x, point.x), std::max(max.y, point.y));
    }
    return Rect::FromEdges(min.x, min.y, max.x, max.y).Inflated(kPavedTouchMeters);
}

PavedTaggedRing TagRing(const AreaRing& ring, PavedEdgeSource source, uint32 index)
{
    return PavedTaggedRing{ring, std::vector<PavedEdge>(ring.size(), PavedEdge{source, index}), TouchBoundsOf(ring)};
}

PavedTaggedRing BuildRibbon(const PavedArmPath& path, float32 stop, uint32 arm)
{
    PavedTaggedRing ribbon;
    std::size_t kept = 0;
    while (kept < path.GetStationCount() && path.GetAlong(kept) < stop - kPavedTouchMeters)
        ++kept;
    const PavedArmSection end = path.SectionAt(stop);
    const bool open = path.GoesOnPast(stop);
    for (std::size_t i = 0; i < kept; ++i)
        ribbon.Ring.push_back(path.GetSection(i).Right);
    ribbon.Ring.push_back(end.Right);
    ribbon.Ring.push_back(end.Left);
    for (std::size_t i = kept; i-- > 0;)
        ribbon.Ring.push_back(path.GetSection(i).Left);
    ribbon.Edges.assign(kept, PavedEdge{PavedEdgeSource::ArmSide, arm});
    ribbon.Edges.push_back(PavedEdge{open ? PavedEdgeSource::Open : PavedEdgeSource::ArmEnd, arm});
    ribbon.Edges.insert(ribbon.Edges.end(), kept, PavedEdge{PavedEdgeSource::ArmSide, arm});
    ribbon.Edges.push_back(PavedEdge{PavedEdgeSource::ArmEnd, arm});
    ribbon.Bounds = TouchBoundsOf(ribbon.Ring);
    return ribbon;
}

bool SegmentMeetsRing(const Vector2& a, const Vector2& b, const PavedTaggedRing& ring)
{
    if (ring.Ring.empty() || !TouchBoundsOf(std::array<Vector2, 2>{a, b}).Overlaps(ring.Bounds))
        return false;
    if (Mathematics::PointInPolygon((a + b) * 0.5f, ring.Ring))
        return true;
    for (std::size_t i = 0; i < ring.Ring.size(); ++i)
    {
        if (SegmentDistance(a, b, ring.Ring[i], ring.Ring[(i + 1) % ring.Ring.size()]) <= kPavedTouchMeters)
            return true;
    }
    return false;
}

bool RingsMeet(const PavedTaggedRing& first, const PavedTaggedRing& second)
{
    if (!first.Bounds.Overlaps(second.Bounds))
        return false;
    for (std::size_t i = 0; i < first.Ring.size(); ++i)
    {
        if (SegmentMeetsRing(first.Ring[i], first.Ring[(i + 1) % first.Ring.size()], second))
            return true;
    }
    return !second.Ring.empty() && Mathematics::PointInPolygon(second.Ring.front(), first.Ring);
}

} // namespace GameEngine::SplineGeometry
