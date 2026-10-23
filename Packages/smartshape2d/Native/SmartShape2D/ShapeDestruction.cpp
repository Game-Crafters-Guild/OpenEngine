#include "SmartShape2D/ShapeDestruction.h"

#include "SmartShape2D/ShapeMesh.h"
#include "SmartShape2D/SmartShapeCut.h"
#include "Assets/ModelAsset.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

namespace GameEngine::SmartShape2D
{
namespace
{
using Mathematics::Vector2;
using Polygon = std::vector<Vertex>;
constexpr float kMinimumCutArea = 1.0e-8f;

float Side(Vector2 a, Vector2 b, const Vertex& vertex)
{
    return (b.x - a.x) * (vertex.Position[1] - a.y) -
           (b.y - a.y) * (vertex.Position[0] - a.x);
}

Vertex InterpolateVertex(const Vertex& a, const Vertex& b, float fraction)
{
    Vertex result = a;
    for (size_t axis = 0; axis < 3; ++axis)
    {
        result.Position[axis] += (b.Position[axis] - a.Position[axis]) * fraction;
        result.Normal[axis] += (b.Normal[axis] - a.Normal[axis]) * fraction;
    }
    for (size_t axis = 0; axis < 2; ++axis)
        result.TexCoords[axis] += (b.TexCoords[axis] - a.TexCoords[axis]) * fraction;
    for (size_t axis = 0; axis < 4; ++axis)
        result.Tangent[axis] += (b.Tangent[axis] - a.Tangent[axis]) * fraction;
    return result;
}

void SplitPolygon(const Polygon& polygon, Vector2 a, Vector2 b, Polygon& inside, Polygon& outside)
{
    inside.clear();
    outside.clear();
    for (size_t index = 0; index < polygon.size(); ++index)
    {
        const auto& current = polygon[index];
        const auto& next = polygon[(index + 1) % polygon.size()];
        const float currentSide = Side(a, b, current);
        const float nextSide = Side(a, b, next);
        (currentSide >= 0.0f ? inside : outside).push_back(current);
        if ((currentSide < 0.0f && nextSide > 0.0f) || (currentSide > 0.0f && nextSide < 0.0f))
        {
            const auto intersection = InterpolateVertex(current, next, currentSide / (currentSide - nextSide));
            inside.push_back(intersection);
            outside.push_back(intersection);
        }
        else if (currentSide == 0.0f)
            outside.push_back(current);
    }
}

void AppendPolygon(Mesh& mesh, const Polygon& polygon)
{
    if (polygon.size() < 3)
        return;
    const auto base = static_cast<uint32>(mesh.Vertices.size());
    mesh.Vertices.insert(mesh.Vertices.end(), polygon.begin(), polygon.end());
    for (size_t index = 1; index + 1 < polygon.size(); ++index)
    {
        const Vector2 a{polygon[0].Position[0], polygon[0].Position[1]};
        const Vector2 b{polygon[index].Position[0], polygon[index].Position[1]};
        if (std::abs(Side(a, b, polygon[index + 1])) <= kMinimumCutArea)
            continue;
        mesh.Indices.insert(mesh.Indices.end(), {base, base + static_cast<uint32>(index),
            base + static_cast<uint32>(index + 1)});
    }
}

bool Overlaps(const Polygon& polygon, const ShapeCut& cut)
{
    Vector2 minimum{polygon[0].Position[0], polygon[0].Position[1]};
    Vector2 maximum = minimum;
    for (const auto& vertex : polygon)
    {
        minimum.x = std::min(minimum.x, vertex.Position[0]);
        minimum.y = std::min(minimum.y, vertex.Position[1]);
        maximum.x = std::max(maximum.x, vertex.Position[0]);
        maximum.y = std::max(maximum.y, vertex.Position[1]);
    }
    Vector2 cutMinimum = cut.front();
    Vector2 cutMaximum = cutMinimum;
    for (const auto& point : cut)
    {
        cutMinimum.x = std::min(cutMinimum.x, point.x);
        cutMinimum.y = std::min(cutMinimum.y, point.y);
        cutMaximum.x = std::max(cutMaximum.x, point.x);
        cutMaximum.y = std::max(cutMaximum.y, point.y);
    }
    return minimum.x < cutMaximum.x && maximum.x > cutMinimum.x &&
           minimum.y < cutMaximum.y && maximum.y > cutMinimum.y;
}
}

std::vector<ShapeCut> BuildShapeCuts(std::span<const Components::SmartShapeCut> cuts)
{
    constexpr size_t kExplosionSegments = 32;
    std::vector<ShapeCut> result;
    for (const auto& cut : cuts)
    {
        const float area = cut.AxisX.x * cut.AxisY.y - cut.AxisX.y * cut.AxisY.x;
        if (!std::isfinite(cut.Center.x) || !std::isfinite(cut.Center.y) ||
            !std::isfinite(area) || std::abs(area) < kMinimumCutArea)
            continue;
        ShapeCut contour;
        contour.reserve(kExplosionSegments);
        for (size_t index = 0; index < kExplosionSegments; ++index)
        {
            const float angle = 2.0f * std::numbers::pi_v<float> * static_cast<float>(index) /
                                static_cast<float>(kExplosionSegments);
            contour.push_back(cut.Center + cut.AxisX * std::cos(angle) + cut.AxisY * std::sin(angle));
        }
        if (area < 0.0f)
            std::reverse(contour.begin(), contour.end());
        result.push_back(std::move(contour));
    }
    return result;
}

void CutShapeMesh(Mesh& mesh, std::span<const ShapeCut> cuts)
{
    for (const auto& cut : cuts)
    {
        if (cut.size() < 3 || mesh.Indices.empty())
            continue;
        Mesh result;
        Polygon inside;
        Polygon outside;
        for (size_t index = 0; index < mesh.Indices.size(); index += 3)
        {
            Polygon remaining{mesh.Vertices[mesh.Indices[index]], mesh.Vertices[mesh.Indices[index + 1]],
                              mesh.Vertices[mesh.Indices[index + 2]]};
            if (!Overlaps(remaining, cut))
            {
                AppendPolygon(result, remaining);
                continue;
            }
            // Each rejected half-plane is disjoint from the remaining polygon.
            // Triangles minus a convex cut therefore need no hole bridging.
            for (size_t edge = 0; edge < cut.size() && remaining.size() >= 3; ++edge)
            {
                SplitPolygon(remaining, cut[edge], cut[(edge + 1) % cut.size()], inside, outside);
                AppendPolygon(result, outside);
                remaining.swap(inside);
            }
        }
        mesh.Vertices = std::move(result.Vertices);
        mesh.Indices = std::move(result.Indices);
    }
    ComputeShapeBounds(mesh);
}
}
