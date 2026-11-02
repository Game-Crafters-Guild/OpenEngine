#include "PavingMesh.h"

#include "Mathematics/Geometry.h"
#include "Mathematics/Interpolation.h"
#include "PavingRoadUV.h"
#include "SplineGeometry/AreaGround.h"
#include "SplineLayout/PieceEntity.h"
#include "Types/FlatMap.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace GameEngine::SplineRecipes
{
namespace
{

using Mathematics::Vector2;
using Mathematics::Vector3;

// The phase spans this many meters on each axis: past any material's tile.
constexpr float32 kPatternPhaseRangeMeters = 64.0f;
// The R2 sequence's steps (the plastic number's inverse powers) in 32-bit fixed point: the product
// with a seed wraps to the step's fractional part exactly, so every seed keeps its own phase.
constexpr uint32 kPhaseStepU = 0xC13FA9A9u;
constexpr uint32 kPhaseStepV = 0x91E10DA5u;

// The lid's triangles after the cuts, with the road whose mapping each takes.
struct LidPieces
{
    // The ground's lid points, then the cut points.
    std::vector<Vector3> Points;
    // Wound so cross(b - a, c - a) points up.
    std::vector<uint32> Triangles;
    std::vector<uint32> TriangleRoad;
};

// The road index of a triangle that takes the lid's own mapping.
constexpr uint32 kOwnMapping = ~0u;
// A corner less than this outside the edge, in meters, counts as inside. The cut then lands on that
// corner and can leave a zero-area triangle, which draws nothing.
constexpr float32 kCutSnapMeters = 1.0e-3f;

Vector2 GroundAxis(const Mathematics::Matrix4x4& world, int column, const Vector2& fallback)
{
    const Vector3 axis = world.TransformPoint(column == 0 ? Vector3(1, 0, 0) : Vector3(0, 0, 1)) -
                         world.TransformPoint(Vector3(0, 0, 0));
    const Vector2 flat(axis.x, axis.z);
    const float32 length = flat.Length();
    return length > 1.0e-6f ? flat / length : fallback;
}

// The lid's points and its triangles wound up, none claimed.
LidPieces UpFacingPieces(const SplineGeometry::AreaGround& ground)
{
    LidPieces pieces;
    pieces.Points = ground.LidPoints;
    pieces.Triangles.reserve(ground.LidTriangles.size());
    for (size_t t = 0; t + 2 < ground.LidTriangles.size(); t += 3)
    {
        const uint32 a = ground.LidTriangles[t];
        uint32 b = ground.LidTriangles[t + 1];
        uint32 c = ground.LidTriangles[t + 2];
        if (Vector3::Cross(pieces.Points[b] - pieces.Points[a], pieces.Points[c] - pieces.Points[a]).y < 0.0f)
            std::swap(b, c);
        pieces.Triangles.insert(pieces.Triangles.end(), {a, b, c});
    }
    pieces.TriangleRoad.assign(pieces.Triangles.size() / 3u, kOwnMapping);
    return pieces;
}

// The point where edge (inside, outside) crosses the zero of `margin`: made once per edge and
// shared by the triangles on both sides of it.
uint32 CutPoint(LidPieces& pieces, std::vector<float32>& margin, FlatMap<uint64, uint32>& cuts, uint32 inside,
                uint32 outside)
{
    const uint64 key = (static_cast<uint64>(std::min(inside, outside)) << 32) | std::max(inside, outside);
    if (const uint32* found = cuts.Find(key))
        return *found;
    const float32 t = Math::Clamp01(margin[inside] / (margin[inside] - margin[outside]));
    const uint32 point = static_cast<uint32>(pieces.Points.size());
    pieces.Points.push_back(pieces.Points[inside] + (pieces.Points[outside] - pieces.Points[inside]) * t);
    margin.push_back(0.0f);
    cuts.InsertOrAssign(key, point);
    return point;
}

// Writes the fan of `polygon` (three or four corners, convex, wound up) as triangles of `road`: the
// first into triangle `slot`, the rest appended.
void EmitPolygon(LidPieces& pieces, std::span<const uint32> polygon, uint32 road, size_t slot)
{
    for (size_t k = 1; k + 1 < polygon.size(); ++k)
    {
        const uint32 corners[3] = {polygon[0], polygon[k], polygon[k + 1]};
        if (k == 1)
        {
            std::copy(corners, corners + 3, pieces.Triangles.begin() + static_cast<std::ptrdiff_t>(slot * 3u));
            pieces.TriangleRoad[slot] = road;
            continue;
        }
        pieces.Triangles.insert(pieces.Triangles.end(), corners, corners + 3);
        pieces.TriangleRoad.push_back(road);
    }
}

// Meters from `xz` to the nearest edge of `outlines`: positive outside all of them, negative inside one.
float32 OutsideOutlines(std::span<const SplineGeometry::AreaRing> outlines, const Vector2& xz)
{
    float32 outside = std::numeric_limits<float32>::max();
    for (const SplineGeometry::AreaRing& ring : outlines)
    {
        float32 nearest = std::numeric_limits<float32>::max();
        for (size_t i = 0; i < ring.size(); ++i)
            nearest = std::min(nearest, Mathematics::DistanceToSegment(xz, ring[i], ring[(i + 1) % ring.size()]));
        outside = std::min(outside, Mathematics::PointInPolygon(xz, ring) ? -nearest : nearest);
    }
    return outside;
}

// Gives every unclaimed triangle within `reach` meters of road `index`'s ribbon and outside `outlines`
// to that road; a triangle the edge crosses is cut along it, its part outside left unclaimed.
void ClaimNearRoad(LidPieces& pieces, const PavingRoadUV& road, uint32 index, float32 reach,
                   std::span<const SplineGeometry::AreaRing> outlines)
{
    std::vector<float32> margin(pieces.Points.size());
    for (size_t p = 0; p < pieces.Points.size(); ++p)
    {
        const Vector2 xz(pieces.Points[p].x, pieces.Points[p].z);
        margin[p] = std::min(LocateOnRoad(road, xz).Inside + reach, OutsideOutlines(outlines, xz));
    }
    FlatMap<uint64, uint32> cuts;
    const size_t triangleCount = pieces.TriangleRoad.size();
    for (size_t t = 0; t < triangleCount; ++t)
    {
        if (pieces.TriangleRoad[t] != kOwnMapping)
            continue;
        const uint32 corners[3] = {pieces.Triangles[t * 3u], pieces.Triangles[t * 3u + 1u], pieces.Triangles[t * 3u + 2u]};
        bool in[3];
        uint32 inCount = 0;
        for (uint32 k = 0; k < 3; ++k)
        {
            in[k] = margin[corners[k]] >= -kCutSnapMeters;
            inCount += in[k] ? 1u : 0u;
        }
        if (inCount == 0u)
            continue;
        if (inCount == 3u)
        {
            pieces.TriangleRoad[t] = index;
            continue;
        }
        // The edge crosses the triangle: one side is a triangle, the other a quad.
        uint32 inside[4];
        uint32 outside[4];
        uint32 insideCount = 0;
        uint32 outsideCount = 0;
        for (uint32 k = 0; k < 3; ++k)
        {
            const uint32 next = (k + 1u) % 3u;
            (in[k] ? inside[insideCount++] : outside[outsideCount++]) = corners[k];
            if (in[k] == in[next])
                continue;
            const uint32 cut = in[k] ? CutPoint(pieces, margin, cuts, corners[k], corners[next])
                                     : CutPoint(pieces, margin, cuts, corners[next], corners[k]);
            inside[insideCount++] = cut;
            outside[outsideCount++] = cut;
        }
        EmitPolygon(pieces, std::span<const uint32>(inside, insideCount), index, t);
        pieces.Triangles.insert(pieces.Triangles.end(), {0u, 0u, 0u});
        pieces.TriangleRoad.push_back(kOwnMapping);
        EmitPolygon(pieces, std::span<const uint32>(outside, outsideCount), kOwnMapping,
                    pieces.TriangleRoad.size() - 1u);
    }
}

// The widest the road's top gets, in meters.
float32 RoadWidth(const PavingRoadUV& road)
{
    float32 width = 0.0f;
    for (size_t i = 0; i < road.HalfLeft.size(); ++i)
        width = std::max(width, road.HalfLeft[i] + road.HalfRight[i]);
    return width;
}

// The lid's triangles cut and claimed: each road's ribbon in claim order, then one road width past
// each ribbon in the same order. No road claims inside `outlines`, so a square's seam lies on its rim.
LidPieces ClaimedPieces(const SplineGeometry::AreaGround& ground, std::span<const PavingRoadUV> roads,
                        std::span<const SplineGeometry::AreaRing> outlines)
{
    LidPieces pieces = UpFacingPieces(ground);
    for (uint32 r = 0; r < roads.size(); ++r)
        ClaimNearRoad(pieces, roads[r], r, 0.0f, outlines);
    for (uint32 r = 0; r < roads.size(); ++r)
        ClaimNearRoad(pieces, roads[r], r, RoadWidth(roads[r]), outlines);
    return pieces;
}

} // namespace

Vector2 PavingPatternPhase(uint32 seed)
{
    const auto fraction = [](uint32 fixedPoint) { return std::ldexp(static_cast<float32>(fixedPoint), -32); };
    return Vector2(fraction(seed * kPhaseStepU), fraction(seed * kPhaseStepV)) * kPatternPhaseRangeMeters;
}

PavingLidMesh BuildPavingLidMesh(const SplineGeometry::AreaGround& ground, std::span<const PavingRoadUV> roads,
                                 std::span<const SplineGeometry::AreaRing> outlines,
                                 const Mathematics::Matrix4x4& ownerWorld, const Vector2& phase)
{
    PavingLidMesh mesh;
    if (ground.LidPoints.empty() || ground.LidTriangles.size() < 3u)
        return mesh;
    const Mathematics::Matrix4x4 toLocal = SplineLayout::InvertPlacerWorld(ownerWorld.Data());
    const Vector2 uAxis = GroundAxis(ownerWorld, 0, Vector2(1.0f, 0.0f));
    const Vector2 vAxis = GroundAxis(ownerWorld, 2, Vector2(0.0f, 1.0f));
    const LidPieces pieces = ClaimedPieces(ground, roads, outlines);

    // Normals average over every triangle at a point, so a cut adds no shading seam.
    std::vector<Vector3> normals(pieces.Points.size(), Vector3(0.0f, 0.0f, 0.0f));
    for (size_t t = 0; t + 2 < pieces.Triangles.size(); t += 3)
    {
        const uint32 a = pieces.Triangles[t];
        const uint32 b = pieces.Triangles[t + 1];
        const uint32 c = pieces.Triangles[t + 2];
        const Vector3 face = Vector3::Cross(pieces.Points[b] - pieces.Points[a], pieces.Points[c] - pieces.Points[a]);
        normals[a] = normals[a] + face;
        normals[b] = normals[b] + face;
        normals[c] = normals[c] + face;
    }

    // One vertex per point and mapping that a triangle uses: a ring sample the lid leaves out has
    // none, and a point on a cut has one on each side.
    constexpr uint32 kUnused = ~0u;
    const size_t mappings = roads.size() + 1u;
    std::vector<uint32> vertexOf(pieces.Points.size() * mappings, kUnused);
    std::vector<std::pair<uint32, uint32>> pointAndRoad;
    mesh.Indices.reserve(pieces.Triangles.size());
    for (size_t i = 0; i < pieces.Triangles.size(); ++i)
    {
        const uint32 point = pieces.Triangles[i];
        const uint32 road = pieces.TriangleRoad[i / 3u];
        const size_t slot = point * mappings + (road == kOwnMapping ? roads.size() : road);
        if (vertexOf[slot] == kUnused)
        {
            vertexOf[slot] = static_cast<uint32>(pointAndRoad.size());
            pointAndRoad.emplace_back(point, road);
        }
        mesh.Indices.push_back(vertexOf[slot]);
    }

    const Vector3 localOrigin = toLocal.TransformPoint(Vector3(0.0f, 0.0f, 0.0f));
    mesh.Vertices.resize(pointAndRoad.size());
    for (size_t v = 0; v < pointAndRoad.size(); ++v)
    {
        const auto [pointIndex, road] = pointAndRoad[v];
        const Vector3& point = pieces.Points[pointIndex];
        const float32 length = normals[pointIndex].Length();
        const Vector3 normal = length > 0.0f ? normals[pointIndex] / length : Vector3(0.0f, 1.0f, 0.0f);
        const Vector2 xz(point.x, point.z);
        // The tangent is U's direction on the ground lifted onto the normal's plane: under a road,
        // its rising arc length as on its strip; else the owner's x axis.
        Vector2 uDirection = uAxis;
        Vector2 uv = Vector2(Vector2::Dot(xz, uAxis), Vector2::Dot(xz, vAxis)) + phase;
        if (road != kOwnMapping)
        {
            const PavingRoadPoint located = LocateOnRoad(roads[road], xz);
            uv = located.UV;
            uDirection = located.Forward;
        }
        const Vector3 flatU(uDirection.x, 0.0f, uDirection.y);
        Vector3 tangent = flatU - normal * Vector3::Dot(flatU, normal);
        tangent = tangent.Length() > 1.0e-6f ? tangent.Normalize() : flatU;

        SplineGeometry::SplineVertex& vertex = mesh.Vertices[v];
        vertex.Position = toLocal.TransformPoint(point);
        vertex.Normal = (toLocal.TransformPoint(normal) - localOrigin).NormalizeOrZero();
        const Vector3 localTangent = (toLocal.TransformPoint(tangent) - localOrigin).NormalizeOrZero();
        vertex.Tangent = Mathematics::Vector4(localTangent.x, localTangent.y, localTangent.z, 1.0f);
        vertex.UV = uv;
        if (v == 0)
        {
            mesh.MinBounds = vertex.Position;
            mesh.MaxBounds = vertex.Position;
        }
        mesh.MinBounds = Vector3(std::min(mesh.MinBounds.x, vertex.Position.x), std::min(mesh.MinBounds.y, vertex.Position.y),
                                 std::min(mesh.MinBounds.z, vertex.Position.z));
        mesh.MaxBounds = Vector3(std::max(mesh.MaxBounds.x, vertex.Position.x), std::max(mesh.MaxBounds.y, vertex.Position.y),
                                 std::max(mesh.MaxBounds.z, vertex.Position.z));
    }
    return mesh;
}

} // namespace GameEngine::SplineRecipes
