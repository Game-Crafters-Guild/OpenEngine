#include "SplineGeometry/PavedFootprint.h"

#include "PavedArmPath.h"
#include "PavedStopSolver.h"
#include "PavedTaggedRing.h"

#include "Mathematics/Geometry.h"
#include "Mathematics/Rect.h"
#include "Types/DisjointSets.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace GameEngine::SplineGeometry
{

using Mathematics::DistanceToSegment;
using Mathematics::Rect;
using Mathematics::Vector2;

namespace
{

// A ring narrower than this on average (twice its area over its perimeter) encloses nothing: the
// mark-up outline rule's bound.
constexpr float32 kMinMeanWidthMeters = 0.01f;
// An arm narrower than this at every station has no ribbon.
constexpr float32 kMinArmWidthMeters = 0.01f;

bool IsSliverRing(std::span<const Vector2> ring)
{
    if (ring.size() < 3)
        return true;
    float32 perimeter = 0.0f;
    for (std::size_t i = 0; i < ring.size(); ++i)
        perimeter += (ring[(i + 1) % ring.size()] - ring[i]).Length();
    return !(std::abs(Mathematics::PolygonDoubledSignedArea(ring)) >= kMinMeanWidthMeters * perimeter);
}

bool IsSliverArm(const PavedArm& arm)
{
    const bool hasLength = std::any_of(arm.Stations.begin(), arm.Stations.end(), [&](const PavedArmStation& station) {
        return (station.Position - arm.Node).Length() > kPavedTouchMeters;
    });
    if (!hasLength)
        return true;
    for (const PavedArmStation& station : arm.Stations)
    {
        if (station.HalfWidthLeft + station.HalfWidthRight >= kMinArmWidthMeters)
            return false;
    }
    return true;
}

// The input edges and points a region's boundary is matched against.
struct RegionInputs
{
    std::vector<const PavedTaggedRing*> Rings;
    std::vector<Vector2> Points;
};

// The source of the input edge segment ab lies on within kPavedTouchMeters; an Open edge wins, so
// no border crosses an approach. New when none holds it.
PavedEdge MatchEdge(const Vector2& a, const Vector2& b, const RegionInputs& inputs)
{
    const Rect bounds = TouchBoundsOf(std::array<Vector2, 2>{a, b});
    PavedEdge match;
    for (const PavedTaggedRing* ring : inputs.Rings)
    {
        if (!bounds.Overlaps(ring->Bounds))
            continue;
        for (std::size_t i = 0; i < ring->Ring.size(); ++i)
        {
            const Vector2& c = ring->Ring[i];
            const Vector2& d = ring->Ring[(i + 1) % ring->Ring.size()];
            if (DistanceToSegment(a, c, d) > kPavedTouchMeters || DistanceToSegment(b, c, d) > kPavedTouchMeters)
                continue;
            if (ring->Edges[i].Source == PavedEdgeSource::Open)
                return ring->Edges[i];
            if (match.Source == PavedEdgeSource::New)
                match = ring->Edges[i];
        }
    }
    return match;
}

bool IsInputPoint(const Vector2& point, const RegionInputs& inputs)
{
    return std::any_of(inputs.Points.begin(), inputs.Points.end(),
                       [&](const Vector2& input) { return (point - input).Length() <= kPavedTouchMeters; });
}

// `ring` with every input point that lies inside one of its edges inserted there, so each edge lies
// on one input edge; `isNew` marks the ring's own points that are no input point.
AreaRing SplitAtInputPoints(const AreaRing& ring, const RegionInputs& inputs, std::vector<uint8>& isNew)
{
    AreaRing split;
    isNew.clear();
    std::vector<std::pair<float32, Vector2>> inside;
    for (std::size_t i = 0; i < ring.size(); ++i)
    {
        const Vector2& a = ring[i];
        const Vector2& b = ring[(i + 1) % ring.size()];
        split.push_back(a);
        isNew.push_back(IsInputPoint(a, inputs) ? 0u : 1u);
        const Vector2 along = b - a;
        const float32 lengthSquared = Vector2::Dot(along, along);
        inside.clear();
        for (const Vector2& point : inputs.Points)
        {
            if ((point - a).Length() <= kPavedTouchMeters || (point - b).Length() <= kPavedTouchMeters ||
                DistanceToSegment(point, a, b) > kPavedTouchMeters)
                continue;
            inside.emplace_back(Vector2::Dot(point - a, along) / lengthSquared, point);
        }
        std::sort(inside.begin(), inside.end(),
                  [](const auto& first, const auto& second) { return first.first < second.first; });
        for (const auto& [t, point] : inside)
        {
            if ((point - split.back()).Length() <= kPavedTouchMeters)
                continue;
            split.push_back(point);
            isNew.push_back(0u);
        }
    }
    return split;
}

// A boundary edge tagged Open.
struct OpenEdge
{
    Vector2 A;
    Vector2 B;
};

// The runs of one boundary ring, cut at its Open edges (the whole ring when it has none); the Open
// edges appended to `openEdges`.
void AppendRuns(const AreaRing& ring, bool hole, const RegionInputs& inputs, std::vector<PavedBoundaryRun>& runs,
                std::vector<OpenEdge>& openEdges)
{
    std::vector<uint8> isNew;
    const AreaRing points = SplitAtInputPoints(ring, inputs, isNew);
    const std::size_t count = points.size();
    std::vector<PavedEdge> edges;
    edges.reserve(count);
    std::size_t firstOpen = count;
    for (std::size_t i = 0; i < count; ++i)
    {
        edges.push_back(MatchEdge(points[i], points[(i + 1) % count], inputs));
        if (edges[i].Source != PavedEdgeSource::Open)
            continue;
        openEdges.push_back(OpenEdge{points[i], points[(i + 1) % count]});
        firstOpen = std::min(firstOpen, i);
    }
    if (firstOpen == count)
    {
        runs.push_back(PavedBoundaryRun{points, std::move(edges), std::move(isNew), true, hole});
        return;
    }
    // Once round the ring from the edge after the first Open one.
    PavedBoundaryRun run;
    run.Hole = hole;
    for (std::size_t step = 1; step <= count; ++step)
    {
        const std::size_t edge = (firstOpen + step) % count;
        if (edges[edge].Source == PavedEdgeSource::Open)
        {
            if (!run.Edges.empty())
            {
                run.Points.push_back(points[edge]);
                run.NewPoints.push_back(isNew[edge]);
                runs.push_back(std::move(run));
            }
            run = PavedBoundaryRun{};
            run.Hole = hole;
            continue;
        }
        run.Points.push_back(points[edge]);
        run.NewPoints.push_back(isNew[edge]);
        run.Edges.push_back(edges[edge]);
    }
}

// The rings of `input` that are not slivers, tagged; a sliver's ring is left empty and reported.
std::vector<PavedTaggedRing> TagRings(std::span<const AreaRing> rings, PavedEdgeSource source,
                                 PavedFootprintIssueKind sliver, std::vector<PavedFootprintIssue>& issues)
{
    std::vector<PavedTaggedRing> tagged;
    tagged.reserve(rings.size());
    for (uint32 index = 0; index < rings.size(); ++index)
    {
        if (IsSliverRing(rings[index]))
        {
            issues.push_back(PavedFootprintIssue{sliver, index});
            tagged.emplace_back();
            continue;
        }
        tagged.push_back(TagRing(rings[index], source, index));
    }
    return tagged;
}

} // namespace

PavedFootprint BuildPavedFootprint(const PavedFootprintInput& input)
{
    PavedFootprint footprint;
    const uint32 outlineCount = static_cast<uint32>(input.Outlines.size());
    const uint32 armCount = static_cast<uint32>(input.Arms.size());
    std::vector<PavedTaggedRing> outlines =
        TagRings(input.Outlines, PavedEdgeSource::Outline, PavedFootprintIssueKind::SliverOutline, footprint.Issues);
    std::vector<uint8> armValid(armCount, 0u);
    for (uint32 arm = 0; arm < armCount; ++arm)
    {
        armValid[arm] = IsSliverArm(input.Arms[arm]) ? 0u : 1u;
        if (!armValid[arm])
            footprint.Issues.push_back(PavedFootprintIssue{PavedFootprintIssueKind::SliverArm, arm});
    }
    const std::vector<PavedTaggedRing> excludes =
        TagRings(input.Excludes, PavedEdgeSource::Exclude, PavedFootprintIssueKind::SliverExclude, footprint.Issues);
    std::vector<AreaRing> cuts;
    for (const PavedTaggedRing& exclude : excludes)
    {
        if (!exclude.Ring.empty())
            cuts.push_back(exclude.Ring);
    }

    PavedStopSolver solver(input.Arms, armValid, std::move(outlines));
    solver.Solve();

    // Items: the outlines, then the arms.
    const uint32 itemCount = outlineCount + armCount;
    DisjointSets sets(itemCount);
    for (uint32 a = 0; a < itemCount; ++a)
    {
        for (uint32 b = a + 1; b < itemCount; ++b)
        {
            const PavedTaggedRing& first = solver.GetItem(a);
            const PavedTaggedRing& second = solver.GetItem(b);
            if (!first.Ring.empty() && !second.Ring.empty() && sets.Find(a) != sets.Find(b) && RingsMeet(first, second))
                sets.Join(a, b);
        }
    }
    std::vector<uint32> regionOfRoot(itemCount, kNoPavedRegion);
    for (uint32 item = 0; item < itemCount; ++item)
    {
        if (solver.GetItem(item).Ring.empty())
            continue;
        uint32& region = regionOfRoot[sets.Find(item)];
        if (region == kNoPavedRegion)
        {
            region = static_cast<uint32>(footprint.Regions.size());
            footprint.Regions.emplace_back();
        }
        if (item < outlineCount)
            footprint.Regions[region].Outlines.push_back(item);
        else
            footprint.Regions[region].Arms.push_back(item - outlineCount);
    }

    footprint.Arms.resize(armCount);
    for (uint32 regionIndex = 0; regionIndex < footprint.Regions.size(); ++regionIndex)
    {
        PavedRegion& region = footprint.Regions[regionIndex];
        RegionInputs inputs;
        for (const uint32 outline : region.Outlines)
            inputs.Rings.push_back(&solver.GetOutline(outline));
        for (const uint32 arm : region.Arms)
            inputs.Rings.push_back(&solver.GetRibbon(arm));
        std::vector<AreaRing> unions;
        for (const PavedTaggedRing* ring : inputs.Rings)
            unions.push_back(ring->Ring);
        for (const PavedTaggedRing& exclude : excludes)
        {
            if (!exclude.Ring.empty())
                inputs.Rings.push_back(&exclude);
        }
        for (const PavedTaggedRing* ring : inputs.Rings)
            inputs.Points.insert(inputs.Points.end(), ring->Ring.begin(), ring->Ring.end());

        region.Pieces = CombineTouchingAreas(unions, cuts, kPavedTouchMeters, kMinMeanWidthMeters);
        std::vector<OpenEdge> openEdges;
        for (const AreaPiece& piece : region.Pieces)
        {
            AppendRuns(piece.Outer, false, inputs, region.Runs, openEdges);
            for (const AreaRing& hole : piece.Holes)
                AppendRuns(hole, true, inputs, region.Runs, openEdges);
        }
        for (const uint32 arm : region.Arms)
        {
            const PavedArmPath& path = solver.GetPath(arm);
            const PavedArmSection end = path.SectionAt(solver.GetStop(arm));
            PavedArmStop& stop = footprint.Arms[arm];
            stop.Region = regionIndex;
            stop.NodeDistance = path.RoadDistance(path.GetSection(0).Distance);
            stop.StopDistance = path.RoadDistance(end.Distance);
            stop.OpenLeft = end.Left;
            stop.OpenRight = end.Right;
            // Open when Open edges on the boundary lie on this arm's far cap: another arm of the
            // road that stops at the same place shares them.
            const bool goesOn = path.GoesOnPast(solver.GetStop(arm));
            float32 openLength = 0.0f;
            for (const OpenEdge& edge : openEdges)
            {
                if (DistanceToSegment(edge.A, end.Left, end.Right) <= kPavedTouchMeters &&
                    DistanceToSegment(edge.B, end.Left, end.Right) <= kPavedTouchMeters)
                    openLength += (edge.B - edge.A).Length();
            }
            stop.Open = goesOn && openLength > kPavedTouchMeters;
        }
    }
    return footprint;
}

} // namespace GameEngine::SplineGeometry
