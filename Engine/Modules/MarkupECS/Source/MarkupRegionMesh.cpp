#include "MarkupECS/MarkupRegionMesh.h"

#include "MarkupECS/MarkupRegionOutline.h"
#include "Mathematics/Geometry.h"
#include "SplineGeometry/AreaBoolean.h"

#include <algorithm>
#include <cmath>

namespace GameEngine::MarkupECS
{

using Mathematics::Vector2;
using Mathematics::Vector3;
using SplineGeometry::AreaGround;
using SplineGeometry::GroundSampler;

namespace
{

Vector2 GroundXZ(const Vector3& point) { return Vector2(point.x, point.z); }

// The mark-up display's lid: split to kLidMinEdgeMeters or the extent over
// kLidEdgeExtentDivisions, at most kMaxLidTriangles; rings sampled `spacing` apart and at most
// kMaxRegionKnots a ring, as the drape caps a spline.
SplineGeometry::AreaGroundSettings LidSettings(float32 spacing)
{
    SplineGeometry::AreaGroundSettings settings;
    settings.RingSpacingMeters = spacing;
    settings.MaxRingSamples = kMaxRegionKnots;
    settings.MaxLidEdgeMeters = kLidMinEdgeMeters;
    settings.MaxLidEdgeExtentFraction = 1.0f / kLidEdgeExtentDivisions;
    settings.MaxLidTriangles = kMaxLidTriangles;
    return settings;
}

// Sets the bounds around the ground's rings.
void SetBounds(MarkupRegionGround& ground)
{
    if (ground.Outline.empty())
        return;
    Vector3 min = ground.Outline.front();
    Vector3 max = min;
    for (const Vector3& point : ground.Outline)
    {
        min = Vector3(std::min(min.x, point.x), std::min(min.y, point.y), std::min(min.z, point.z));
        max = Vector3(std::max(max.x, point.x), std::max(max.y, point.y), std::max(max.z, point.z));
    }
    ground.Center = (min + max) * 0.5f;
    ground.Top = max.y;
    for (const Vector3& point : ground.Outline)
        ground.Radius = std::max(ground.Radius, (point - ground.Center).Length());
}

// Sets the ground's one ring, kept as given with no lid, and the bounds around it.
void SetLidlessRing(MarkupRegionGround& ground, std::vector<Vector3> ring)
{
    ground.OutlineXZ.reserve(ring.size());
    for (const Vector3& point : ring)
        ground.OutlineXZ.push_back(GroundXZ(point));
    ground.Outline = std::move(ring);
    ground.RingEnds.push_back(static_cast<uint32>(ground.Outline.size()));
    SetBounds(ground);
}

// A closed base outline's merged samples `base` turned counter-clockwise from above, with the
// region's label point and whether the outline crosses itself read from them.
std::vector<Vector3> ReadClosedBase(std::vector<Vector3> base, const GroundSampler& groundAt,
                                    MarkupRegionGround& ground)
{
    std::vector<Vector2> ring2D;
    ring2D.reserve(base.size());
    for (const Vector3& point : base)
        ring2D.push_back(GroundXZ(point));
    if (Mathematics::PolygonDoubledSignedArea(ring2D) < 0.0f)
    {
        std::reverse(base.begin(), base.end());
        std::reverse(ring2D.begin(), ring2D.end());
    }
    const Vector2 label = RegionLabelPoint(ring2D);
    ground.LabelGround = Vector3(label.x, groundAt(label), label.y);
    // The drape caps the samples at kMaxRegionKnots, so the outline rule's count bound holds; a
    // crossing, or a sliver with no area, has no lid.
    ground.Crosses = CheckRegionOutline(ring2D).has_value();
    return base;
}

// The point halfway along an open polyline's length.
Vector3 HalfwayAlong(const std::vector<Vector3>& line)
{
    float32 remaining = 0.5f * PolylineLength(line);
    for (std::size_t i = 1; i < line.size(); ++i)
    {
        const float32 segment = (line[i] - line[i - 1]).Length();
        if (segment >= remaining && segment > 0.0f)
            return line[i - 1] + (line[i] - line[i - 1]) * (remaining / segment);
        remaining -= segment;
    }
    return line.back();
}

void AppendVertex(MarkupRegionMesh& mesh, const Vector3& position, float32 rim)
{
    mesh.Vertices.push_back(MarkupRegionVertex{position, rim});
}

// Reads the ground across a path's band at every sample (EdgeGround).
void SamplePathEdges(MarkupRegionGround& ground, const GroundSampler& groundAt)
{
    const std::vector<Vector3>& line = ground.Outline;
    ground.EdgeGround.reserve(line.size() * kPathEdgeOffsets.size() * 2);
    for (std::size_t i = 0; i < line.size(); ++i)
    {
        const Vector3 across = PathAcross(line, i);
        for (const float32 side : {-1.0f, 1.0f})
        {
            for (const float32 offset : kPathEdgeOffsets)
                ground.EdgeGround.push_back(groundAt(GroundXZ(line[i] + across * (side * offset))));
        }
    }
}

} // namespace

float32 PolylineLength(std::span<const Vector3> line)
{
    float32 length = 0.0f;
    for (std::size_t i = 1; i < line.size(); ++i)
        length += (line[i] - line[i - 1]).Length();
    return length;
}

Vector3 PathAcross(std::span<const Vector3> line, std::size_t i)
{
    const Vector3& previous = line[i == 0 ? 0 : i - 1];
    const Vector3& next = line[std::min(i + 1, line.size() - 1)];
    const Vector3 along = next - previous;
    return Vector3(along.z, 0.0f, -along.x).NormalizeOrZero();
}

float32 PathEdgeHeight(const MarkupRegionGround& ground, std::size_t i, float32 side, float32 halfWidth)
{
    constexpr std::size_t kOffsets = kPathEdgeOffsets.size();
    if (ground.EdgeGround.size() != ground.Outline.size() * kOffsets * 2)
        return ground.Outline[i].y;
    const float32* heights = ground.EdgeGround.data() + (i * 2 + (side > 0.0f ? 1 : 0)) * kOffsets;
    if (halfWidth <= kPathEdgeOffsets.front())
        return heights[0];
    for (std::size_t k = 1; k < kOffsets; ++k)
    {
        if (halfWidth <= kPathEdgeOffsets[k])
        {
            const float32 t = (halfWidth - kPathEdgeOffsets[k - 1]) / (kPathEdgeOffsets[k] - kPathEdgeOffsets[k - 1]);
            return heights[k - 1] + (heights[k] - heights[k - 1]) * t;
        }
    }
    return heights[kOffsets - 1];
}

MarkupRegionGround BuildMarkupRegionGround(std::span<const Vector3> outline, bool closed,
                                           const GroundSampler& groundAt)
{
    MarkupRegionGround ground;
    std::vector<Vector3> merged = SplineGeometry::MergeGroundSamples(outline, closed);
    if (merged.empty())
        return ground;
    ground.Closed = closed && merged.size() >= kMinRegionKnots;
    if (!ground.Closed)
    {
        ground.LabelGround = HalfwayAlong(merged);
        SetLidlessRing(ground, std::move(merged));
        if (!closed)
            SamplePathEdges(ground, groundAt);
        return ground;
    }
    std::vector<Vector3> base = ReadClosedBase(std::move(merged), groundAt, ground);
    if (ground.Crosses)
    {
        SetLidlessRing(ground, std::move(base));
        return ground;
    }
    static_cast<AreaGround&>(ground) =
        SplineGeometry::BuildRingGround(std::move(base), LidSettings(0.0f), groundAt); // the drape sampled the ring
    SetBounds(ground);
    return ground;
}

MarkupRegionGround BuildMarkupRegionAreaGround(std::span<const Vector3> outline,
                                               std::span<const SplineGeometry::AreaPiece> area, float32 spacing,
                                               const GroundSampler& groundAt)
{
    MarkupRegionGround ground;
    std::vector<Vector3> base = SplineGeometry::MergeGroundSamples(outline, true);
    if (base.size() < kMinRegionKnots)
        return BuildMarkupRegionGround(outline, true, groundAt);
    ground.Closed = true;
    ReadClosedBase(std::move(base), groundAt, ground);
    if (ground.Crosses)
        return BuildMarkupRegionGround(outline, true, groundAt);
    static_cast<AreaGround&>(ground) = SplineGeometry::BuildAreaGround(area, LidSettings(spacing), groundAt);
    SetBounds(ground);
    return ground;
}

MarkupRegionMesh BuildMarkupRegionMesh(const MarkupRegionGround& ground, float32 extrudeHeight)
{
    MarkupRegionMesh mesh;
    const Vector3 lift(0.0f, extrudeHeight, 0.0f);
    mesh.Label = ground.LabelGround + lift;
    if (ground.Outline.empty() || !ground.Closed)
        return mesh;

    // The rim coordinate is the distance to the top edge over the rim's width.
    const float32 rimWidth = std::max(kMarkupRegionRimWidthFraction * extrudeHeight, 1.0e-3f);
    const float32 bottomRim = extrudeHeight / rimWidth;
    mesh.Vertices.reserve(ground.Outline.size() * 6 + ground.LidTriangles.size());
    for (std::size_t r = 0; r < ground.GetRingCount(); ++r)
    {
        const std::span<const Vector3> ring = ground.GetRing(r);
        for (std::size_t i = 0; i < ring.size(); ++i)
        {
            // Quad from sample i to sample i + 1, wound to face out of its ring (out of the area on
            // a counter-clockwise outer ring, into the hole on a clockwise one): seen from outside
            // each triangle turns counter-clockwise, as markup_glow.vert winds a box's faces.
            const Vector3& from = ring[i];
            const Vector3& to = ring[(i + 1) % ring.size()];
            AppendVertex(mesh, to, bottomRim);
            AppendVertex(mesh, from + lift, 0.0f);
            AppendVertex(mesh, from, bottomRim);
            AppendVertex(mesh, to, bottomRim);
            AppendVertex(mesh, to + lift, 0.0f);
            AppendVertex(mesh, from + lift, 0.0f);
        }
    }
    mesh.WallVertexCount = mesh.Vertices.size();
    for (const uint32 index : ground.LidTriangles)
        AppendVertex(mesh, ground.LidPoints[index] + lift, ground.LidEdgeDistance[index] / rimWidth);
    return mesh;
}

} // namespace GameEngine::MarkupECS
