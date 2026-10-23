#include "SmartShape2D/ShapeCollision.h"

#include "SmartShape2D/ShapeGeometry.h"

#include <algorithm>
#include <cmath>
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
            if (i <= static_cast<size_t>(ShapePart::Left) || i >= static_cast<size_t>(ShapePart::Count))
                ExtrudeMesh(geometry.Parts[i], depth, result);
        }
    }
    return result;
}
}
