#include "SmartShape2D/ShapeMesh.h"

#include "Mathematics/Geometry.h"

#include <algorithm>
#include <array>

namespace GameEngine::SmartShape2D
{
using Mathematics::Vector2;

void AppendShapeVertex(Mesh& mesh, Vector2 position, Vector2 uv, float depth)
{
    Vertex vertex{};
    vertex.Position[0] = position.x;
    vertex.Position[1] = position.y;
    vertex.Position[2] = depth;
    vertex.Normal[2] = -1.0f;
    vertex.Tangent[0] = 1.0f;
    vertex.Tangent[3] = -1.0f;
    vertex.TexCoords[0] = uv.x;
    vertex.TexCoords[1] = uv.y;
    mesh.Vertices.push_back(vertex);
}

void AppendShapeQuad(Mesh& mesh, Vector2 a, Vector2 b, Vector2 c, Vector2 d,
                float startU, float endU, float depth, bool flip)
{
    const auto base = static_cast<uint32>(mesh.Vertices.size());
    const float top = flip ? 1.0f : 0.0f;
    const float bottom = 1.0f - top;
    AppendShapeVertex(mesh, a, {startU, top}, depth);
    AppendShapeVertex(mesh, b, {endU, top}, depth);
    AppendShapeVertex(mesh, c, {endU, bottom}, depth);
    AppendShapeVertex(mesh, d, {startU, bottom}, depth);
    const float firstArea = Mathematics::PolygonDoubledSignedArea(std::array{a, b, c});
    const float secondArea = Mathematics::PolygonDoubledSignedArea(std::array{a, c, d});
    // Welding a tight bend can make a quad concave. Its interior diagonal is the one
    // whose triangles have matching winding; the other diagonal overlaps outside the quad.
    if ((firstArea < 0.0f && secondArea > 0.0f) || (firstArea > 0.0f && secondArea < 0.0f))
    {
        const float area = Mathematics::PolygonDoubledSignedArea(std::array{a, b, d});
        if (area < 0.0f)
            mesh.Indices.insert(mesh.Indices.end(), {base, base + 1, base + 3, base + 1, base + 2, base + 3});
        else
            mesh.Indices.insert(mesh.Indices.end(), {base, base + 3, base + 1, base + 1, base + 3, base + 2});
    }
    else if (firstArea < 0.0f || secondArea < 0.0f)
        mesh.Indices.insert(mesh.Indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    else
        mesh.Indices.insert(mesh.Indices.end(), {base, base + 2, base + 1, base, base + 3, base + 2});
}

void ComputeShapeBounds(Mesh& mesh)
{
    if (mesh.Vertices.empty())
        return;
    for (size_t axis = 0; axis < 3; ++axis)
    {
        mesh.MinBounds[axis] = mesh.Vertices.front().Position[axis];
        mesh.MaxBounds[axis] = mesh.MinBounds[axis];
        for (const auto& vertex : mesh.Vertices)
        {
            mesh.MinBounds[axis] = std::min(mesh.MinBounds[axis], vertex.Position[axis]);
            mesh.MaxBounds[axis] = std::max(mesh.MaxBounds[axis], vertex.Position[axis]);
        }
    }
}

}
