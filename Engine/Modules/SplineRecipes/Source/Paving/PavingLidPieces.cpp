#include "PavingLidPieces.h"

#include "Mathematics/Geometry.h"
#include "Mathematics/Interpolation.h"
#include "Types/FlatMap.h"

#include <algorithm>
#include <limits>

namespace GameEngine::SplineRecipes
{
namespace
{

// A corner less than this outside the edge, in meters, counts as inside. The cut then lands on that
// corner and can leave a zero-area triangle, which draws nothing.
constexpr float32 kCutSnapMeters = 1.0e-3f;

// The point where edge (inside, outside) crosses the zero of `margin`.
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

// Writes the fan of `polygon` (three or four corners, convex, wound up) as triangles of `mapping`: the
// first into triangle `slot`, the rest appended.
void EmitPolygon(LidPieces& pieces, std::span<const uint32> polygon, uint32 mapping, size_t slot)
{
    for (size_t k = 1; k + 1 < polygon.size(); ++k)
    {
        const uint32 corners[3] = {polygon[0], polygon[k], polygon[k + 1]};
        if (k == 1)
        {
            std::copy(corners, corners + 3, pieces.Triangles.begin() + static_cast<std::ptrdiff_t>(slot * 3u));
            pieces.TriangleMapping[slot] = mapping;
            continue;
        }
        pieces.Triangles.insert(pieces.Triangles.end(), corners, corners + 3);
        pieces.TriangleMapping.push_back(mapping);
    }
}

} // namespace

void CutAlongZero(LidPieces& pieces, std::vector<float32>& margin, uint32 claim, uint32 index)
{
    FlatMap<uint64, uint32> cuts;
    const size_t triangleCount = pieces.TriangleMapping.size();
    for (size_t t = 0; t < triangleCount; ++t)
    {
        if (pieces.TriangleMapping[t] != claim)
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
            pieces.TriangleMapping[t] = index;
            continue;
        }
        // The zero crosses the triangle: one side is a triangle, the other a quad.
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
        pieces.TriangleMapping.push_back(claim);
        EmitPolygon(pieces, std::span<const uint32>(outside, outsideCount), claim, pieces.TriangleMapping.size() - 1u);
    }
}

float32 OutsideOutlines(std::span<const SplineGeometry::AreaRing> outlines, const Mathematics::Vector2& xz)
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

} // namespace GameEngine::SplineRecipes
