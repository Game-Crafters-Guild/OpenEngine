#include "SplineGeometry/AreaGround.h"

#include "Mathematics/Geometry.h"
#include "Mathematics/Interpolation.h"
#include "Mathematics/Triangulation.h"
#include "SplineGeometry/AreaBoolean.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace GameEngine::SplineGeometry
{

using Mathematics::Vector2;
using Mathematics::Vector3;

namespace
{

// Outline samples closer than this are one sample.
constexpr float32 kSameSampleMeters = 1.0e-3f;
constexpr uint32 kNoTriangle = std::numeric_limits<uint32>::max();

Vector2 GroundXZ(const Vector3& point) { return Vector2(point.x, point.z); }

float32 LengthSquared(const Vector2& v) { return v.x * v.x + v.y * v.y; }

// The distance from `point` to the closed ring's outline on the ground plane, meters.
float32 DistanceToRing(const Vector2& point, std::span<const Vector2> ring)
{
    const Vector3 lifted(point.x, 0.0f, point.y);
    float32 nearest = std::numeric_limits<float32>::max();
    for (std::size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++)
    {
        const Vector3 closest = Mathematics::ClosestPointOnSegment(lifted, Vector3(ring[j].x, 0.0f, ring[j].y),
                                                                   Vector3(ring[i].x, 0.0f, ring[i].y));
        nearest = std::min(nearest, (closest - lifted).Length());
    }
    return nearest;
}

// The lid's triangulation on the ground plane, relative to the first ring's first point (so a
// region far from the origin keeps its precision): the rings ear-clipped, then every edge longer
// than the limit split at its midpoint in both triangles that share it, so no triangle meets
// another at a T and the raised lid has no cracks.
class LidRefiner
{
  public:
    // `points` are the rings' samples at their `heights`, ring after ring (`ringEnds`, one past
    // each ring's last), `triangles` the ear-clipped kept corners; the split stops at
    // `maxTriangles`.
    LidRefiner(std::vector<Vector2> points, std::vector<uint32> ringEnds, std::vector<uint32> triangles,
               std::vector<float32> heights, std::size_t maxTriangles)
        : m_Points(std::move(points)), m_Heights(std::move(heights)), m_RingEnds(std::move(ringEnds)),
          m_MaxTriangles(maxTriangles)
    {
        m_RingOf.resize(m_Points.size(), kNoRing);
        m_RingPosition.resize(m_Points.size(), 0.0f);
        for (uint32 ring = 0; ring < m_RingEnds.size(); ++ring)
        {
            for (uint32 i = RingStart(ring); i < m_RingEnds[ring]; ++i)
            {
                m_RingOf[i] = ring;
                m_RingPosition[i] = static_cast<float32>(i - RingStart(ring));
            }
        }
        std::vector<EdgeRecord> edges;
        edges.reserve(triangles.size());
        for (std::size_t t = 0; t + 2 < triangles.size(); t += 3)
        {
            const uint32 index = static_cast<uint32>(m_Triangles.size());
            m_Triangles.push_back({triangles[t], triangles[t + 1], triangles[t + 2]});
            for (uint32 k = 0; k < 3; ++k)
                edges.push_back({Key(triangles[t + k], triangles[t + (k + 1) % 3]), index, k});
        }
        LinkNeighbors(std::move(edges));
    }

    // Splits until no edge is longer than `maxEdge`, or the triangle cap is reached, by
    // longest-edge propagation (Rivara's LEPP): a triangle's longest edge is split only once it is
    // the longest edge of the triangle across it too, after that neighbor has been split along
    // its own. So the split never cuts a short edge of a sliver, and the triangle count stays near
    // the area over the limit squared. Each pass for a triangle splits an edge, so it adds a
    // triangle toward the cap, or it ends that triangle's refinement: an edge that more than two
    // triangles share cannot be split in all of them, so it is not split.
    void Refine(float32 maxEdge)
    {
        const float32 limitSquared = maxEdge * maxEdge;
        for (std::size_t t = 0; t < m_Triangles.size() && m_Triangles.size() < m_MaxTriangles; ++t)
        {
            while (m_Triangles.size() < m_MaxTriangles)
            {
                const uint32 triangle = static_cast<uint32>(t);
                const uint32 side = LongestSide(triangle);
                const std::array<uint32, 3>& corners = m_Triangles[triangle];
                if (LengthSquared(m_Points[corners[(side + 1) % 3]] - m_Points[corners[side]]) <= limitSquared)
                    break;
                const std::pair<uint32, uint32> terminal = TerminalSide(triangle, side);
                if (!SplitSide(terminal.first, terminal.second))
                    break;
            }
        }
    }

    // Splits every outline edge at the ring samples it spans (the collinear ones left out of the ear
    // clip), so the lid's edge passes through every sample the walls stand on and the two meet
    // without a crack: the triangle on such an edge becomes a fan from its opposite corner (and a
    // fan triangle that still holds another such edge, at a corner of the area, fans again).
    void InsertSkippedSamples()
    {
        std::vector<std::array<uint32, 3>> refined;
        refined.reserve(m_Triangles.size() * 2);
        for (uint32 t = 0; t < m_Triangles.size(); ++t)
            FanSkippedSamples(m_Triangles[t], t, refined);
        m_Triangles = std::move(refined);
    }

    const std::vector<Vector2>& GetPoints() const { return m_Points; }
    // NaN for a point inside the area, which takes the ground sampler's height.
    const std::vector<float32>& GetHeights() const { return m_Heights; }
    std::vector<uint32> FlattenTriangles() const
    {
        std::vector<uint32> flat;
        flat.reserve(m_Triangles.size() * 3);
        for (const std::array<uint32, 3>& triangle : m_Triangles)
            flat.insert(flat.end(), triangle.begin(), triangle.end());
        return flat;
    }

  private:
    // Side `Side` of triangle `Triangle` (from corner `Side` to the next), under its edge's key.
    struct EdgeRecord
    {
        uint64_t Key = 0;
        uint32 Triangle = 0;
        uint32 Side = 0;
    };

    static bool EdgeRecordLess(const EdgeRecord& a, const EdgeRecord& b)
    {
        return a.Key != b.Key ? a.Key < b.Key : a.Triangle < b.Triangle;
    }

    // Fills m_Neighbors from every triangle's three sides: one sort puts the sides of one edge
    // next to each other. An edge of one triangle lies on a ring; an edge of more than two is
    // marked kManyTriangles on each of them.
    void LinkNeighbors(std::vector<EdgeRecord> edges)
    {
        std::sort(edges.begin(), edges.end(), EdgeRecordLess);
        m_Neighbors.assign(m_Triangles.size(), {kNoTriangle, kNoTriangle, kNoTriangle});
        for (std::size_t first = 0, end = 0; first < edges.size(); first = end)
        {
            end = first + 1;
            while (end < edges.size() && edges[end].Key == edges[first].Key)
                ++end;
            if (end - first == 2)
            {
                m_Neighbors[edges[first].Triangle][edges[first].Side] = edges[first + 1].Triangle;
                m_Neighbors[edges[first + 1].Triangle][edges[first + 1].Side] = edges[first].Triangle;
            }
            else if (end - first > 2)
            {
                for (std::size_t e = first; e < end; ++e)
                    m_Neighbors[edges[e].Triangle][edges[e].Side] = kManyTriangles;
            }
        }
    }

    uint64_t SideKey(uint32 triangle, uint32 side) const
    {
        const std::array<uint32, 3>& corners = m_Triangles[triangle];
        return Key(corners[side], corners[(side + 1) % 3]);
    }

    // The side of `triangle` on the edge under `key` with `neighbor` across it.
    uint32 SideToward(uint32 triangle, uint32 neighbor, uint64_t key) const
    {
        uint32 side = 0;
        while (side < 2 && !(m_Neighbors[triangle][side] == neighbor && SideKey(triangle, side) == key))
            ++side;
        return side;
    }

    // A triangle's longest side; ties go to the smaller edge key, so the triangles either side
    // of an edge agree on whether it is their longest.
    uint32 LongestSide(uint32 triangle) const
    {
        const std::array<uint32, 3>& corners = m_Triangles[triangle];
        uint32 longest = 0;
        float32 longestSquared = -1.0f;
        for (uint32 k = 0; k < 3; ++k)
        {
            const float32 lengthSquared = LengthSquared(m_Points[corners[(k + 1) % 3]] - m_Points[corners[k]]);
            if (lengthSquared > longestSquared ||
                (lengthSquared == longestSquared && SideKey(triangle, k) < SideKey(triangle, longest)))
            {
                longestSquared = lengthSquared;
                longest = k;
            }
        }
        return longest;
    }

    // The end of the longest-edge path from side `side` of `triangle`: across each longest side to
    // the neighbor while the neighbor's longest side is another one. The (triangle, side) where the
    // path stops is the longest side of both triangles that share it, or has no triangle across.
    std::pair<uint32, uint32> TerminalSide(uint32 triangle, uint32 side) const
    {
        for (std::size_t step = 0; step < m_Triangles.size(); ++step)
        {
            const uint32 neighbor = m_Neighbors[triangle][side];
            if (neighbor == kNoTriangle || neighbor == kManyTriangles)
                return {triangle, side};
            const uint32 next = LongestSide(neighbor);
            if (SideKey(neighbor, next) == SideKey(triangle, side))
                return {triangle, side};
            triangle = neighbor;
            side = next;
        }
        return {triangle, side};
    }

    uint32 RingStart(uint32 ring) const { return ring == 0 ? 0u : m_RingEnds[ring - 1]; }
    uint32 RingSize(uint32 ring) const { return m_RingEnds[ring] - RingStart(ring); }

    // The position on ring `ring` of `point`, the midpoint of the ring's edge (u, v): the sample
    // segment between their positions it lies on, and how far along it.
    float32 RingPositionOn(uint32 ring, uint32 u, uint32 v, const Vector2& point) const
    {
        const uint32 start = RingStart(ring);
        const std::size_t count = RingSize(ring);
        float32 from = m_RingPosition[u];
        float32 to = m_RingPosition[v];
        if (to < from)
            std::swap(from, to);
        // The edge runs the short way round the ring.
        if (to - from > 0.5f * static_cast<float32>(count))
        {
            std::swap(from, to);
            to += static_cast<float32>(count);
        }
        float32 best = from;
        float32 bestDistance = std::numeric_limits<float32>::max();
        for (float32 segment = std::floor(from); segment < to; segment += 1.0f)
        {
            const Vector2& a = m_Points[start + static_cast<std::size_t>(segment) % count];
            const Vector2& b = m_Points[start + (static_cast<std::size_t>(segment) + 1) % count];
            const Vector2 ab = b - a;
            const float32 lengthSquared = LengthSquared(ab);
            const float32 s = lengthSquared > 0.0f
                                  ? Math::Clamp01(((point.x - a.x) * ab.x + (point.y - a.y) * ab.y) / lengthSquared)
                                  : 0.0f;
            const float32 distance = LengthSquared(a + ab * s - point);
            if (distance < bestDistance)
            {
                bestDistance = distance;
                best = std::fmod(segment + s, static_cast<float32>(count));
            }
        }
        return best;
    }

    // Ring `ring`'s height at a position on it, between its two samples.
    float32 RingHeightAt(uint32 ring, float32 position) const
    {
        const uint32 start = RingStart(ring);
        const std::size_t count = RingSize(ring);
        const std::size_t a = static_cast<std::size_t>(position) % count;
        const std::size_t b = (a + 1) % count;
        const float32 s = position - std::floor(position);
        return m_Heights[start + a] + (m_Heights[start + b] - m_Heights[start + a]) * s;
    }

    // `triangle` is a fan piece of the refined triangle `source`, at first the triangle itself.
    void FanSkippedSamples(const std::array<uint32, 3>& triangle, uint32 source,
                           std::vector<std::array<uint32, 3>>& out) const
    {
        std::vector<uint32> samples;
        for (std::size_t k = 0; k < 3; ++k)
        {
            const uint32 a = triangle[k];
            const uint32 b = triangle[(k + 1) % 3];
            const uint32 c = triangle[(k + 2) % 3];
            samples.clear();
            AppendSkippedSamples(a, b, source, samples);
            if (samples.empty())
                continue;
            uint32 previous = a;
            samples.push_back(b);
            for (const uint32 sample : samples)
            {
                FanSkippedSamples({previous, sample, c}, source, out);
                previous = sample;
            }
            return;
        }
        out.push_back(triangle);
    }

    // The ring's edge (a, b), when a and b lie on one ring and no triangle is across it
    // (`neighbor`): the ring's index, else kNoRing (an edge inside the area, a bridge to a hole
    // among them).
    uint32 RingOfEdge(uint32 a, uint32 b, uint32 neighbor) const
    {
        return neighbor == kNoTriangle && m_RingOf[a] != kNoRing && m_RingOf[a] == m_RingOf[b] ? m_RingOf[a]
                                                                                              : kNoRing;
    }

    // The ring samples strictly between ring edge (a, b)'s ends, in ring order, appended to
    // `out`; none for an edge that is not a side of the refined triangle `source`, or that lies
    // inside the area.
    void AppendSkippedSamples(uint32 a, uint32 b, uint32 source, std::vector<uint32>& out) const
    {
        const std::array<uint32, 3>& corners = m_Triangles[source];
        uint32 side = 0;
        while (side < 3 && !(corners[side] == a && corners[(side + 1) % 3] == b))
            ++side;
        if (side == 3)
            return;
        const uint32 ringIndex = RingOfEdge(a, b, m_Neighbors[source][side]);
        if (ringIndex == kNoRing)
            return;
        const uint32 start = RingStart(ringIndex);
        const std::size_t count = RingSize(ringIndex);
        // The edge spans the short way round the ring, in either direction.
        const float32 ring = static_cast<float32>(count);
        const float32 forward = std::fmod(m_RingPosition[b] - m_RingPosition[a] + ring, ring);
        const bool reversed = forward > 0.5f * ring;
        const float32 from = reversed ? m_RingPosition[b] : m_RingPosition[a];
        const float32 to = from + (reversed ? ring - forward : forward);
        const std::size_t first = out.size();
        for (float32 sample = std::floor(from) + 1.0f; sample < to; sample += 1.0f)
            out.push_back(static_cast<uint32>(start + static_cast<std::size_t>(sample) % count));
        if (reversed)
            std::reverse(out.begin() + static_cast<std::ptrdiff_t>(first), out.end());
    }

    static uint64_t Key(uint32 a, uint32 b)
    {
        return (static_cast<uint64_t>(std::min(a, b)) << 32) | std::max(a, b);
    }

    // Splits side `side` (a, b) of `triangle` (a, b, c) at `middle`: the triangle becomes
    // (a, middle, c) and (middle, b, c) is appended, its index returned. Side 0 of each half lies on
    // the split edge, with no triangle across until the caller links it.
    uint32 SplitTriangle(uint32 triangle, uint32 side, uint32 middle)
    {
        const std::array<uint32, 3> corners = m_Triangles[triangle];
        const std::array<uint32, 3> across = m_Neighbors[triangle];
        const uint32 b = corners[(side + 1) % 3];
        const uint32 c = corners[(side + 2) % 3];
        const uint32 added = static_cast<uint32>(m_Triangles.size());
        m_Triangles[triangle] = {corners[side], middle, c};
        m_Triangles.push_back({middle, b, c});
        m_Neighbors[triangle] = {kNoTriangle, added, across[(side + 2) % 3]};
        m_Neighbors.push_back({kNoTriangle, across[(side + 1) % 3], triangle});
        const uint32 acrossBC = across[(side + 1) % 3];
        if (acrossBC != kNoTriangle && acrossBC != kManyTriangles)
            m_Neighbors[acrossBC][SideToward(acrossBC, triangle, Key(b, c))] = added;
        return added;
    }

    // Splits side `side` (u, v) of `triangle` at its midpoint, in it and in the triangle across
    // it; a midpoint on a ring (a side with no triangle across) takes the height on the line
    // between its ends, so the lid's edge stays on the walls' top edge. False, and nothing changes,
    // when more than two triangles share the edge.
    bool SplitSide(uint32 triangle, uint32 side)
    {
        const uint32 neighbor = m_Neighbors[triangle][side];
        if (neighbor == kManyTriangles)
            return false;
        const uint32 u = m_Triangles[triangle][side];
        const uint32 v = m_Triangles[triangle][(side + 1) % 3];
        const uint32 ring = RingOfEdge(u, v, neighbor);
        const uint32 middle = static_cast<uint32>(m_Points.size());
        const Vector2 point = (m_Points[u] + m_Points[v]) * 0.5f;
        m_Points.push_back(point);
        m_RingOf.push_back(ring);
        if (ring != kNoRing)
        {
            // On a ring the point takes the ring's own height there, so the lid's edge stays on the
            // walls' top edge.
            const float32 position = RingPositionOn(ring, u, v, point);
            m_RingPosition.push_back(position);
            m_Heights.push_back(RingHeightAt(ring, position));
        }
        else
        {
            m_RingPosition.push_back(0.0f);
            m_Heights.push_back(std::numeric_limits<float32>::quiet_NaN());
        }
        const uint32 added = SplitTriangle(triangle, side, middle);
        if (neighbor == kNoTriangle)
            return true;
        const uint32 neighborAdded = SplitTriangle(neighbor, SideToward(neighbor, triangle, Key(u, v)), middle);
        // (u, middle) is side 0 of `triangle` and of `neighborAdded`; (middle, v) of `added` and
        // of `neighbor`.
        m_Neighbors[triangle][0] = neighborAdded;
        m_Neighbors[neighborAdded][0] = triangle;
        m_Neighbors[added][0] = neighbor;
        m_Neighbors[neighbor][0] = added;
        return true;
    }

    static constexpr uint32 kManyTriangles = kNoTriangle - 1;
    // The ring of a point inside the area.
    static constexpr uint32 kNoRing = std::numeric_limits<uint32>::max();

    std::vector<Vector2> m_Points;
    std::vector<float32> m_Heights;
    std::vector<uint32> m_RingEnds;
    std::size_t m_MaxTriangles = 0;
    std::vector<uint32> m_RingOf;        // a point's ring, or kNoRing inside the area
    std::vector<float32> m_RingPosition; // on its ring: a sample's index, or the place between two
    std::vector<std::array<uint32, 3>> m_Triangles;
    // Per triangle, the triangle across each side (side k runs from corner k to the next):
    // kNoTriangle across a ring edge, kManyTriangles across an edge more than two triangles share.
    std::vector<std::array<uint32, 3>> m_Neighbors;
};

// The ring's corners: the samples a straight run between two kept samples would pass farther than
// kAreaLidInsetMeters from, so every sample left out lies on an edge between corners.

std::vector<uint32> RingCorners(std::span<const Vector2> ring)
{
    std::vector<uint32> corners{0};
    const std::size_t count = ring.size();
    std::size_t anchor = 0;
    while (anchor < count)
    {
        // The farthest sample `end` such that every sample between `anchor` and it lies on the
        // straight run between the two.
        std::size_t end = anchor + 1;
        while (end < count)
        {
            const std::size_t next = end + 1;
            const Vector2& a = ring[anchor];
            const Vector2& b = ring[next % count];
            const Vector2 ab = b - a;
            const float32 length = std::sqrt(LengthSquared(ab));
            bool straight = length > 0.0f;
            for (std::size_t k = anchor + 1; straight && k < next; ++k)
            {
                const Vector2 ak = ring[k] - a;
                straight = std::abs(ab.x * ak.y - ab.y * ak.x) / length <= kAreaLidInsetMeters;
            }
            if (!straight)
                break;
            end = next;
        }
        if (end >= count)
            break;
        corners.push_back(static_cast<uint32>(end));
        anchor = end;
    }
    return corners;
}

// One piece of the lid: the ground's outer ring and the holes inside it, as ring indices.
struct LidPiece
{
    uint32 Outer = 0;
    std::vector<uint32> Holes;
};

// The rightmost x of ring `ring` in `points` (ring after ring, `ringEnds`).
float32 RingRightmostX(const std::vector<Vector2>& points, const std::vector<uint32>& ringEnds, uint32 ring)
{
    float32 rightmost = -std::numeric_limits<float32>::max();
    for (uint32 i = ring == 0 ? 0u : ringEnds[ring - 1]; i < ringEnds[ring]; ++i)
        rightmost = std::max(rightmost, points[i].x);
    return rightmost;
}

// Ring `ring`'s corners (RingCorners) as indices into `points` (ring after ring, `ringEnds`).
std::vector<uint32> RingCornerIndices(const std::vector<Vector2>& points, const std::vector<uint32>& ringEnds,
                                      uint32 ring)
{
    const uint32 start = ring == 0 ? 0u : ringEnds[ring - 1];
    std::vector<uint32> corners = RingCorners(std::span<const Vector2>(points).subspan(start, ringEnds[ring] - start));
    for (uint32& corner : corners)
        corner += start;
    return corners;
}

// The lid over the ground's simple rings: each piece's outer ring (counter-clockwise from above)
// with its holes (clockwise) bridged in.
void BuildLid(const std::vector<LidPiece>& pieces, const AreaGroundSettings& settings, const GroundSampler& groundAt,
              AreaGround& ground)
{
    const Vector2 origin = ground.OutlineXZ.front();
    std::vector<Vector2> local;
    std::vector<float32> heights;
    local.reserve(ground.Outline.size());
    heights.reserve(ground.Outline.size());
    Vector2 min = origin;
    Vector2 max = origin;
    for (const Vector3& point : ground.Outline)
    {
        local.push_back(GroundXZ(point) - origin);
        heights.push_back(point.y);
        min = Vector2(std::min(min.x, point.x), std::min(min.y, point.z));
        max = Vector2(std::max(max.x, point.x), std::max(max.y, point.z));
    }
    // The ear clip takes the rings' corners only: a straight run of samples would clip into flat
    // slivers the split could never make well-shaped. The samples left out go back into the
    // ring edges after the split.
    std::vector<uint32> triangles;
    for (const LidPiece& piece : pieces)
    {
        std::vector<uint32> polygon = RingCornerIndices(local, ground.RingEnds, piece.Outer);
        std::vector<uint32> holes = piece.Holes;
        // Rightmost hole first, so a later bridge never crosses an earlier one.
        std::sort(holes.begin(), holes.end(), [&](uint32 a, uint32 b) {
            return RingRightmostX(local, ground.RingEnds, a) > RingRightmostX(local, ground.RingEnds, b);
        });
        for (const uint32 hole : holes)
        {
            // A hole with fewer than three corners lies within the inset of one straight run (a
            // lens between two chords) and encloses nothing at the lid's scale. It is not bridged:
            // its slit would leave an edge that three triangles share, and the split would leave
            // the triangles on it coarse.
            const std::vector<uint32> holeCorners = RingCornerIndices(local, ground.RingEnds, hole);
            if (holeCorners.size() >= 3)
                Mathematics::BridgeHole(local, polygon, holeCorners);
        }
        Mathematics::EarClip(local, std::move(polygon), triangles);
    }

    LidRefiner refiner(std::move(local), ground.RingEnds, std::move(triangles), std::move(heights),
                       settings.MaxLidTriangles);
    const float32 extent = std::max(max.x - min.x, max.y - min.y);
    refiner.Refine(std::max(settings.MaxLidEdgeMeters, extent * settings.MaxLidEdgeExtentFraction));
    refiner.InsertSkippedSamples();

    std::vector<Vector2> rings2D;
    rings2D.reserve(ground.OutlineXZ.size());
    for (const Vector2& point : ground.OutlineXZ)
        rings2D.push_back(point - origin);
    const std::vector<Vector2>& points = refiner.GetPoints();
    const std::vector<float32>& pointHeights = refiner.GetHeights();
    ground.LidPoints.reserve(points.size());
    ground.LidEdgeDistance.reserve(points.size());
    for (std::size_t i = 0; i < points.size(); ++i)
    {
        const Vector2 world = points[i] + origin;
        const bool interior = std::isnan(pointHeights[i]);
        ground.LidPoints.emplace_back(world.x, interior ? groundAt(world) : pointHeights[i], world.y);
        // The rings' own points and their edges' midpoints lie on a ring.
        float32 distance = 0.0f;
        if (interior)
        {
            distance = std::numeric_limits<float32>::max();
            for (std::size_t ring = 0; ring < ground.GetRingCount(); ++ring)
            {
                const uint32 start = ring == 0 ? 0u : ground.RingEnds[ring - 1];
                distance = std::min(distance, DistanceToRing(points[i], std::span<const Vector2>(rings2D).subspan(
                                                                            start, ground.RingEnds[ring] - start)));
            }
        }
        ground.LidEdgeDistance.push_back(distance);
    }
    ground.LidTriangles = refiner.FlattenTriangles();
}

// `ring` sampled along the ground: its corners, and between them samples at most the settings'
// spacing apart, the spacing widened so a ring of any length adds at most their sample cap; each
// at `groundAt`'s height, near-duplicates merged.
std::vector<Vector3> SampleAreaRing(std::span<const Vector2> ring, const AreaGroundSettings& settings,
                                    const GroundSampler& groundAt)
{
    float32 perimeter = 0.0f;
    for (std::size_t i = 0; i < ring.size(); ++i)
        perimeter += std::sqrt(LengthSquared(ring[(i + 1) % ring.size()] - ring[i]));
    const float32 step = std::max(settings.RingSpacingMeters,
                                  perimeter / static_cast<float32>(std::max<std::size_t>(settings.MaxRingSamples, 1)));
    std::vector<Vector3> samples;
    for (std::size_t i = 0; i < ring.size(); ++i)
    {
        const Vector2 from = ring[i];
        const Vector2 to = ring[(i + 1) % ring.size()];
        const float32 length = std::sqrt(LengthSquared(to - from));
        const int steps = std::max(1, static_cast<int>(std::ceil(length / step)));
        for (int s = 0; s < steps; ++s)
        {
            const Vector2 point = from + (to - from) * (static_cast<float32>(s) / static_cast<float32>(steps));
            samples.emplace_back(point.x, groundAt(point), point.y);
        }
    }
    return MergeGroundSamples(samples, true);
}

// `ring` sampled (SampleAreaRing) and wound counter-clockwise from above when `outer`, else
// clockwise: the lid's ear clip and split take an outer ring counter-clockwise and a hole
// clockwise, and do not end on a ring wound the other way.
std::vector<Vector3> SampleWoundRing(std::span<const Vector2> ring, bool outer, const AreaGroundSettings& settings,
                                     const GroundSampler& groundAt)
{
    std::vector<Vector3> samples = SampleAreaRing(ring, settings, groundAt);
    if ((Mathematics::PolygonDoubledSignedArea(ring) > 0.0f) != outer)
        std::reverse(samples.begin(), samples.end());
    return samples;
}

// Sets the ground's rings, ring after ring.
void SetRings(AreaGround& ground, std::vector<std::vector<Vector3>> rings)
{
    for (std::vector<Vector3>& ring : rings)
    {
        ground.Outline.insert(ground.Outline.end(), ring.begin(), ring.end());
        ground.RingEnds.push_back(static_cast<uint32>(ground.Outline.size()));
    }
    ground.OutlineXZ.reserve(ground.Outline.size());
    for (const Vector3& point : ground.Outline)
        ground.OutlineXZ.push_back(GroundXZ(point));
}

} // namespace

// The samples with near-duplicates merged, and a closed ring's repeated first point dropped.
std::vector<Vector3> MergeGroundSamples(std::span<const Vector3> samples, bool closed)
{
    std::vector<Vector3> merged;
    merged.reserve(samples.size());
    for (const Vector3& point : samples)
    {
        if (!merged.empty() && LengthSquared(GroundXZ(point) - GroundXZ(merged.back())) <=
                                   kSameSampleMeters * kSameSampleMeters)
            continue;
        merged.push_back(point);
    }
    while (closed && merged.size() > 1 &&
           LengthSquared(GroundXZ(merged.back()) - GroundXZ(merged.front())) <= kSameSampleMeters * kSameSampleMeters)
        merged.pop_back();
    return merged;
}

std::span<const Vector3> AreaGround::GetRing(std::size_t ring) const
{
    const uint32 start = ring == 0 ? 0u : RingEnds[ring - 1];
    return std::span<const Vector3>(Outline).subspan(start, RingEnds[ring] - start);
}

std::span<const Vector2> AreaGround::GetRingXZ(std::size_t ring) const
{
    const uint32 start = ring == 0 ? 0u : RingEnds[ring - 1];
    return std::span<const Vector2>(OutlineXZ).subspan(start, RingEnds[ring] - start);
}

bool AreaGround::Contains(const Vector2& xz) const
{
    bool inside = false;
    for (std::size_t ring = 0; ring < GetRingCount(); ++ring)
        inside = inside != Mathematics::PointInPolygon(xz, GetRingXZ(ring));
    return inside;
}

AreaGround BuildAreaGround(std::span<const AreaPiece> pieces, const AreaGroundSettings& settings,
                           const GroundSampler& groundAt)
{
    AreaGround ground;
    std::vector<std::vector<Vector3>> rings;
    std::vector<LidPiece> lidPieces;
    for (const AreaPiece& piece : pieces)
    {
        LidPiece& lid = lidPieces.emplace_back();
        lid.Outer = static_cast<uint32>(rings.size());
        rings.push_back(SampleWoundRing(piece.Outer, true, settings, groundAt));
        for (const AreaRing& hole : piece.Holes)
        {
            lid.Holes.push_back(static_cast<uint32>(rings.size()));
            rings.push_back(SampleWoundRing(hole, false, settings, groundAt));
        }
    }
    SetRings(ground, std::move(rings));
    if (!lidPieces.empty())
        BuildLid(lidPieces, settings, groundAt, ground);
    return ground;
}

AreaGround BuildRingGround(std::vector<Vector3> ring, const AreaGroundSettings& settings, const GroundSampler& groundAt)
{
    AreaGround ground;
    const bool enclosesArea = ring.size() >= 3;
    SetRings(ground, {std::move(ring)});
    if (enclosesArea)
        BuildLid({LidPiece{}}, settings, groundAt, ground);
    return ground;
}

} // namespace GameEngine::SplineGeometry
