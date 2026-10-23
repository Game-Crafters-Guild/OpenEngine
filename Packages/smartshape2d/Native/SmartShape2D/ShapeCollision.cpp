#include "SmartShape2D/ShapeCollision.h"

#include "SmartShape2D/ShapeGeometry.h"
#include "SmartShape2D/ShapeOffset.h"
#include "SmartShape2D/SmartShape.h"
#include "Mathematics/Geometry.h"
#include "Mathematics/Triangulation.h"
#include "clipper2/clipper.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <utility>
#include <vector>

namespace GameEngine::SmartShape2D
{
namespace
{
void AppendContourFaces(const Clipper2Lib::PolyPathD& contour, Mesh& mesh)
{
    if (!contour.IsHole() && !contour.Polygon().empty())
    {
        std::vector<Mathematics::Vector2> points;
        for (const auto& point : contour.Polygon())
            points.emplace_back(static_cast<float>(point.x), static_cast<float>(point.y));
        std::vector<uint32> polygon(points.size());
        std::iota(polygon.begin(), polygon.end(), 0u);
        for (const auto& hole : contour)
        {
            std::vector<uint32> indices;
            for (const auto& point : hole->Polygon())
            {
                indices.push_back(static_cast<uint32>(points.size()));
                points.emplace_back(static_cast<float>(point.x), static_cast<float>(point.y));
            }
            Mathematics::BridgeHole(points, polygon, indices);
        }
        std::vector<uint32> triangles;
        Mathematics::EarClip(points, std::move(polygon), triangles);
        const auto base = static_cast<uint32>(mesh.Vertices.size());
        for (const auto point : points)
        {
            Vertex vertex{};
            vertex.Position[0] = point.x;
            vertex.Position[1] = point.y;
            mesh.Vertices.push_back(vertex);
        }
        for (size_t index = 0; index < triangles.size(); index += 3)
            mesh.Indices.insert(mesh.Indices.end(), {base + triangles[index],
                base + triangles[index + 2], base + triangles[index + 1]});
    }
    for (const auto& child : contour)
        AppendContourFaces(*child, mesh);
}

void CutCollisionMesh(Mesh& mesh, std::span<const ShapeCut> cuts)
{
    if (cuts.empty() || mesh.Indices.empty())
        return;
    Clipper2Lib::PathsD triangles;
    triangles.reserve(mesh.Indices.size() / 3);
    for (size_t index = 0; index < mesh.Indices.size(); index += 3)
    {
        Clipper2Lib::PathD triangle;
        for (size_t corner = 0; corner < 3; ++corner)
        {
            const auto& vertex = mesh.Vertices[mesh.Indices[index + corner]];
            triangle.emplace_back(vertex.Position[0], vertex.Position[1]);
        }
        triangles.push_back(std::move(triangle));
    }
    Clipper2Lib::PathsD footprints;
    for (const auto& cut : cuts)
    {
        Clipper2Lib::PathD footprint;
        for (const auto point : cut)
            footprint.emplace_back(point.x, point.y);
        footprints.push_back(std::move(footprint));
    }
    // Clip the solid before triangulating: clipping triangles independently leaves
    // T-junctions that would become false interior collision walls during extrusion.
    constexpr int kClipPrecisionDigits = 6;
    Clipper2Lib::ClipperD clipper(kClipPrecisionDigits);
    clipper.AddSubject(triangles);
    clipper.AddClip(footprints);
    Clipper2Lib::PolyTreeD contours;
    clipper.Execute(Clipper2Lib::ClipType::Difference, Clipper2Lib::FillRule::NonZero, contours);
    mesh.Vertices.clear();
    mesh.Indices.clear();
    AppendContourFaces(contours, mesh);
}

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

Physics::MeshShapeDef BuildCollision(const ShapeGeometry& geometry, bool closed, const Components::SmartShape& settings,
                                   std::span<const ShapeCut> cuts)
{
    if (!geometry.Error.empty() || !std::isfinite(settings.CollisionWidth) || settings.CollisionWidth <= 0.0f ||
        !std::isfinite(settings.CollisionOffset) || !std::isfinite(settings.CollisionDepth) || settings.CollisionDepth <= 0.0f)
        return {};
    if (settings.CollisionMode == Components::ShapeCollisionMode::RenderedEdges)
    {
        Physics::MeshShapeDef result;
        for (size_t part = static_cast<size_t>(ShapePart::Top); part < geometry.Parts.size(); ++part)
        {
            if (part < geometry.Kinds.size() && geometry.Kinds[part] != ShapePart::Fill)
            {
                auto mesh = geometry.Parts[part];
                CutCollisionMesh(mesh, cuts);
                ExtrudeMesh(mesh, settings.CollisionDepth, result);
            }
        }
        return result;
    }
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
    CutCollisionMesh(mesh, cuts);
    ExtrudeMesh(mesh, settings.CollisionDepth, result);
    return result;
}

}
