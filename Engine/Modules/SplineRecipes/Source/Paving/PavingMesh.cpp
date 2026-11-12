#include "PavingMesh.h"

#include "PavingLidPieces.h"
#include "PavingRoadUV.h"
#include "PavingSeam.h"
#include "SplineGeometry/AreaGround.h"
#include "SplineLayout/PieceEntity.h"

#include <algorithm>
#include <cmath>

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
    pieces.TriangleMapping.assign(pieces.Triangles.size() / 3u, kOwnMapping);
    return pieces;
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
    CutAlongZero(pieces, margin, kOwnMapping, index);
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

// A vertex slot per mapping: the roads, the own mapping, the courses, the bands.
size_t MappingSlot(uint32 mapping, uint32 roadCount)
{
    if (mapping == kOwnMapping)
        return roadCount;
    return mapping < roadCount ? mapping : mapping + 1u;
}

// The part that draws the triangles of `mapping` (PavingSeams' mapping order).
PavingLidMesh& PartOf(PavingLidParts& parts, uint32 mapping, uint32 roadCount)
{
    if (mapping == kOwnMapping || mapping < roadCount)
        return parts.Lid;
    return mapping < 2u * roadCount ? parts.Courses[mapping - roadCount] : parts.Band;
}

} // namespace

Vector2 PavingPatternPhase(uint32 seed)
{
    const auto fraction = [](uint32 fixedPoint) { return std::ldexp(static_cast<float32>(fixedPoint), -32); };
    return Vector2(fraction(seed * kPhaseStepU), fraction(seed * kPhaseStepV)) * kPatternPhaseRangeMeters;
}

PavingLidParts BuildPavingLidMesh(const SplineGeometry::AreaGround& ground, std::span<const PavingRoadUV> roads,
                                  std::span<const PavingRoadSeam> seams,
                                  std::span<const SplineGeometry::AreaRing> outlines,
                                  const Mathematics::Matrix4x4& ownerWorld, const Vector2& phase,
                                  const PavingSeamMaterial& material)
{
    PavingLidParts parts;
    parts.Courses.resize(roads.size());
    if (ground.LidPoints.empty() || ground.LidTriangles.size() < 3u)
        return parts;
    const Mathematics::Matrix4x4 toLocal = SplineLayout::InvertPlacerWorld(ownerWorld.Data());
    const PavingOwnMapping own{GroundAxis(ownerWorld, 0, Vector2(1.0f, 0.0f)), GroundAxis(ownerWorld, 2, Vector2(0.0f, 1.0f)),
                               phase};
    LidPieces pieces = ClaimedPieces(ground, roads, outlines);
    PavingSeams lidSeams(roads, seams, outlines, own, material);
    lidSeams.Cut(pieces);
    for (uint32 road = 0; road < roads.size(); ++road)
    {
        for (uint32 partner = 0; partner < road; ++partner)
        {
            if (lidSeams.DrawsHardCut(road, partner))
                parts.HardCutSeams.push_back({roads[road].Entity, roads[partner].Entity});
        }
        if (lidSeams.DrawsHardCut(road, kOwnMapping))
            parts.HardCutSeams.push_back({roads[road].Entity, std::nullopt});
    }

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
    // none, and a point on a cut has one on each side. Mappings: the roads, the own mapping, the
    // courses, the bands.
    constexpr uint32 kUnused = ~0u;
    const uint32 roadCount = static_cast<uint32>(roads.size());
    const size_t mappings = 3u * roadCount + 1u;
    std::vector<uint32> vertexOf(pieces.Points.size() * mappings, kUnused);
    const Vector3 localOrigin = toLocal.TransformPoint(Vector3(0.0f, 0.0f, 0.0f));
    for (size_t i = 0; i < pieces.Triangles.size(); ++i)
    {
        const uint32 pointIndex = pieces.Triangles[i];
        const uint32 mapping = pieces.TriangleMapping[i / 3u];
        PavingLidMesh& part = PartOf(parts, mapping, roadCount);
        uint32& vertexIndex = vertexOf[pointIndex * mappings + MappingSlot(mapping, roadCount)];
        if (vertexIndex == kUnused)
        {
            vertexIndex = static_cast<uint32>(part.Vertices.size());
            const Vector3& point = pieces.Points[pointIndex];
            const float32 length = normals[pointIndex].Length();
            const Vector3 normal = length > 0.0f ? normals[pointIndex] / length : Vector3(0.0f, 1.0f, 0.0f);
            const PavingSeamVertex attributes = lidSeams.VertexOf(mapping, Vector2(point.x, point.z));
            // The tangent is U's direction on the ground lifted onto the normal's plane.
            const Vector3 flatU(attributes.UDirection.x, 0.0f, attributes.UDirection.y);
            Vector3 tangent = flatU - normal * Vector3::Dot(flatU, normal);
            tangent = tangent.Length() > 1.0e-6f ? tangent.Normalize() : flatU;

            SplineGeometry::SplineVertex vertex;
            vertex.Position = toLocal.TransformPoint(point);
            vertex.Normal = (toLocal.TransformPoint(normal) - localOrigin).NormalizeOrZero();
            const Vector3 localTangent = (toLocal.TransformPoint(tangent) - localOrigin).NormalizeOrZero();
            vertex.Tangent = Mathematics::Vector4(localTangent.x, localTangent.y, localTangent.z, 1.0f);
            vertex.UV = attributes.UV;
            if (part.Vertices.empty())
            {
                part.MinBounds = vertex.Position;
                part.MaxBounds = vertex.Position;
            }
            part.MinBounds = Vector3(std::min(part.MinBounds.x, vertex.Position.x), std::min(part.MinBounds.y, vertex.Position.y),
                                     std::min(part.MinBounds.z, vertex.Position.z));
            part.MaxBounds = Vector3(std::max(part.MaxBounds.x, vertex.Position.x), std::max(part.MaxBounds.y, vertex.Position.y),
                                     std::max(part.MaxBounds.z, vertex.Position.z));
            part.Vertices.push_back(vertex);
            if (&part != &parts.Lid)
                part.Colors.push_back(attributes.Tint);
            if (&part == &parts.Band)
                part.SecondUV.push_back(attributes.SecondUV);
        }
        part.Indices.push_back(vertexIndex);
    }
    return parts;
}

} // namespace GameEngine::SplineRecipes
