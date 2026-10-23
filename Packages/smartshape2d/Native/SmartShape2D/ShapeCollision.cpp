#include "SmartShape2D/ShapeCollision.h"

#include "SmartShape2D/ShapeGeometry.h"
#include "SmartShape2D/ShapeOffset.h"
#include "SmartShape2D/SmartShape.h"
#include "Mathematics/Geometry.h"
#include "Mathematics/Triangulation.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <utility>
#include <vector>

namespace GameEngine::SmartShape2D
{
namespace
{
void ExtrudeMesh(const Mesh& source, float depth, Physics::MeshShapeDef& destination)
{
    const auto base = static_cast<uint32>(destination.vertices.size());
    const auto count = static_cast<uint32>(source.Vertices.size());
    for (const float z : {-depth * 0.5f, depth * 0.5f})
    {
        for (const auto& vertex : source.Vertices)
            destination.vertices.push_back({vertex.Position[0], vertex.Position[1], z});
    }
    std::vector<std::pair<uint32, uint32>> edges;
    for (size_t i = 0; i < source.Indices.size(); i += 3)
    {
        const auto a = source.Indices[i];
        const auto b = source.Indices[i + 1];
        const auto c = source.Indices[i + 2];
        destination.indices.insert(destination.indices.end(),
            {base + a, base + b, base + c, base + count + a, base + count + c, base + count + b});
        edges.insert(edges.end(), {{a, b}, {b, c}, {c, a}});
    }
    // Only boundary edges need walls. Interior triangulation edges appear in both directions.
    std::sort(edges.begin(), edges.end());
    for (const auto& [a, b] : edges)
    {
        if (std::binary_search(edges.begin(), edges.end(), std::pair{b, a}))
            continue;
        destination.indices.insert(destination.indices.end(),
            {base + a, base + count + a, base + count + b, base + a, base + count + b, base + b});
    }
}
}

Physics::MeshShapeDef BuildCollision(const ShapeGeometry& geometry, bool closed, float depth)
{
    Physics::MeshShapeDef result;
    if (!geometry.Error.empty() || !std::isfinite(depth) || depth <= 0.0f)
        return result;
    if (closed)
        ExtrudeMesh(geometry.Parts[static_cast<size_t>(ShapePart::Fill)], depth, result);
    else
    {
        for (size_t i = static_cast<size_t>(ShapePart::Top); i < geometry.Parts.size(); ++i)
        {
            if (i < geometry.Kinds.size() && geometry.Kinds[i] != ShapePart::Fill)
                ExtrudeMesh(geometry.Parts[i], depth, result);
        }
    }
    return result;
}
Physics::MeshShapeDef BuildCollision(const ShapeGeometry& geometry, bool closed, const Components::SmartShape& settings)
{
    if (!geometry.Error.empty() || !std::isfinite(settings.CollisionWidth) || settings.CollisionWidth <= 0.0f ||
        !std::isfinite(settings.CollisionOffset) || !std::isfinite(settings.CollisionDepth) || settings.CollisionDepth <= 0.0f)
        return {};
    if (settings.CollisionMode == Components::ShapeCollisionMode::RenderedEdges)
        return BuildCollision(geometry, false, settings.CollisionDepth);
    std::vector<Mathematics::Vector2> outline;
    std::vector<Mathematics::Vector2> hole;
    if (!closed)
    {
        const auto shifted = OffsetShapePath(geometry.Outline, settings.CollisionOffset - settings.CollisionWidth * 0.5f);
        outline = BufferShapePath(shifted, settings.CollisionWidth * 0.5f);
    }
    else if (settings.CollisionMode == Components::ShapeCollisionMode::Hollow)
    {
        outline = OffsetShapePolygon(geometry.Outline, settings.CollisionOffset + settings.CollisionWidth * 0.5f);
        hole = OffsetShapePolygon(geometry.Outline, settings.CollisionOffset - settings.CollisionWidth * 0.5f);
    }
    else
        outline = OffsetShapePolygon(geometry.Outline, settings.CollisionOffset);
    if (outline.size() < 3)
        return {};
    if (Mathematics::PolygonDoubledSignedArea(outline) < 0.0f)
        std::reverse(outline.begin(), outline.end());
    if (Mathematics::PolygonDoubledSignedArea(hole) > 0.0f)
        std::reverse(hole.begin(), hole.end());
    std::vector<uint32> polygon(outline.size());
    std::iota(polygon.begin(), polygon.end(), 0u);
    if (!hole.empty())
    {
        std::vector<uint32> holeIndices(hole.size());
        std::iota(holeIndices.begin(), holeIndices.end(), static_cast<uint32>(outline.size()));
        outline.insert(outline.end(), hole.begin(), hole.end());
        Mathematics::BridgeHole(outline, polygon, holeIndices);
    }
    Mesh mesh;
    for (const auto& point : outline)
    {
        Vertex vertex;
        vertex.Position[0] = point.x;
        vertex.Position[1] = point.y;
        mesh.Vertices.push_back(vertex);
    }
    Mathematics::EarClip(outline, std::move(polygon), mesh.Indices);
    for (size_t index = 0; index < mesh.Indices.size(); index += 3)
        std::swap(mesh.Indices[index + 1], mesh.Indices[index + 2]);
    Physics::MeshShapeDef result;
    ExtrudeMesh(mesh, settings.CollisionDepth, result);
    return result;
}

}
