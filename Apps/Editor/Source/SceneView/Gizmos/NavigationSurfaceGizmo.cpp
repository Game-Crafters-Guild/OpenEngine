#include "SceneView/Gizmos/NavigationSurfaceGizmo.h"

#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Mathematics/Vector3.h"
#include "Pathfinding/DetourNavMap.h"
#include "Pathfinding/GridMap.h"
#include "PathfindingECS/Components/NavigationSurface.h"
#include "PathfindingECS/NavigationSurfaceRuntime.h"
#include "SceneView/ComponentGizmoRegistry.h"
#include "SceneView/SceneViewGizmos.h"
#include "Types/Color.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_map>
#include <vector>

namespace GameEngine::Editor::SceneTools
{
namespace
{

using Components::NavigationSurface;
using Mathematics::Vector3;

// One convention in both modes: the walkable surface is a faint blue tint, and its edge (where it
// meets ground an agent cannot walk, or ends) is an opaque line, so an exclusion reads as an
// outlined hole. Blue reads against grass, rock and sand alike; the outline contrasts with it.
const Color kWalkableColor(0.15f, 0.55f, 1.0f, 0.22f);
const Color kBoundaryColor(1.0f, 0.88f, 0.3f, 1.0f);
const Color kBoundsColor(0.15f, 0.55f, 1.0f, 0.9f);
// The outline's width, constant on screen so an exclusion reads at any distance.
constexpr float kBoundaryThickness = 2.5f;
// Metres the overlay floats above what it covers: more than the 0.2 m a baked mesh's height may
// stray from the ground (its detail error at the tallest cell), so the ground does not poke
// through it.
constexpr float kLift = 0.3f;
// The most grid cells one frame draws; a larger grid is drawn every n-th cell.
constexpr uint32 kMaxDrawnCells = 65536;

// One surface's lifted fill triangles and outline segments, rebuilt when what they were built from
// changes. Gizmos draw on the main thread.
struct LiftedSurface
{
    std::array<uint64, 4> Key{};
    std::vector<Vector3> Fill;
    std::vector<Vector3> Outline; // endpoint pairs
};

// The most surfaces whose overlay is kept; past it the cache starts over (a selection of that
// many surfaces at once is not a case worth more).
constexpr size_t kMaxCachedSurfaces = 16;

// The cache of the surface on `entity`, one per surface, so two selected surfaces do not rebuild
// each other's overlay every frame. Keyed by the entity; IsCurrent checks what it was built from.
LiftedSurface& Lifted(ECS::EntityHandle entity)
{
    static std::unordered_map<ECS::EntityHandle, LiftedSurface, ECS::EntityHandleHash> s_Lifted;
    if (s_Lifted.size() >= kMaxCachedSurfaces && !s_Lifted.contains(entity))
        s_Lifted.clear();
    return s_Lifted[entity];
}

// True when the cache was built from `key`; otherwise clears it and adopts the key.
bool IsCurrent(LiftedSurface& lifted, const std::array<uint64, 4>& key)
{
    if (lifted.Key == key)
        return true;
    lifted.Key = key;
    lifted.Fill.clear();
    lifted.Outline.clear();
    return false;
}

Vector3 LiftedVertex(const float32* vertices, uint32 index)
{
    const float32* v = &vertices[static_cast<size_t>(index) * 3];
    return Vector3(v[0], v[1] + kLift, v[2]);
}

void BuildMesh(LiftedSurface& lifted, const Pathfinding::DetourNavMap& mesh)
{
    const uint32 triangles = mesh.GetDebugTriangleCount();
    const float32* vertices = mesh.GetDebugVertices();
    const uint32* indices = mesh.GetDebugIndices();
    if (triangles == 0 || !vertices || !indices)
        return;
    lifted.Fill.reserve(static_cast<size_t>(triangles) * 3);
    for (uint32 i = 0; i < triangles * 3; ++i)
        lifted.Fill.push_back(LiftedVertex(vertices, indices[i]));
    const uint32 edges = mesh.GetDebugBoundaryEdgeCount();
    const uint32* boundary = mesh.GetDebugBoundaryIndices();
    if (edges == 0 || !boundary)
        return;
    lifted.Outline.reserve(static_cast<size_t>(edges) * 2);
    for (uint32 i = 0; i < edges * 2; ++i)
        lifted.Outline.push_back(LiftedVertex(vertices, boundary[i]));
}

// The lifted centre of grid cell (qx, qz) x `stride`.
Vector3 GridCorner(const Pathfinding::GridMap& grid, uint32 stride, uint32 qx, uint32 qz)
{
    const uint32 x = qx * stride;
    const uint32 z = qz * stride;
    float32 wx = 0.0f, wz = 0.0f;
    grid.CellToWorld(x, z, wx, wz);
    return Vector3(wx, grid.GetCellHeight(x, z) + kLift, wz);
}

bool IsQuadDrawn(const std::vector<uint8>& drawn, uint32 quadsX, uint32 quadsZ, int64 qx, int64 qz)
{
    return qx >= 0 && qz >= 0 && qx < quadsX && qz < quadsZ && drawn[static_cast<size_t>(qz) * quadsX + qx] != 0;
}

// A grid is drawn as the surface through its walkable cell centres: a quad joins four walkable
// neighbours (every `stride`-th cell on a large grid), so heights run continuously instead of in
// terraces, and a blocked cell leaves a hole whose edge is outlined.
void BuildGrid(LiftedSurface& lifted, const Pathfinding::GridMap& grid)
{
    const auto& settings = grid.GetSettings();
    const uint32 stride =
        std::max(1u, static_cast<uint32>(std::ceil(std::sqrt(static_cast<float>(grid.GetCellCount()) / kMaxDrawnCells))));
    if (settings.Width <= stride || settings.Depth <= stride)
        return;
    const uint32 quadsX = (settings.Width - 1) / stride;
    const uint32 quadsZ = (settings.Depth - 1) / stride;
    std::vector<uint8> drawn(static_cast<size_t>(quadsX) * quadsZ, 0);
    for (uint32 qz = 0; qz < quadsZ; ++qz)
    {
        for (uint32 qx = 0; qx < quadsX; ++qx)
        {
            if (grid.IsCellBlocked(qx * stride, qz * stride) || grid.IsCellBlocked((qx + 1) * stride, qz * stride) ||
                grid.IsCellBlocked((qx + 1) * stride, (qz + 1) * stride) || grid.IsCellBlocked(qx * stride, (qz + 1) * stride))
                continue;
            drawn[static_cast<size_t>(qz) * quadsX + qx] = 1;
            const Vector3 a = GridCorner(grid, stride, qx, qz), b = GridCorner(grid, stride, qx + 1, qz),
                          c = GridCorner(grid, stride, qx + 1, qz + 1), d = GridCorner(grid, stride, qx, qz + 1);
            lifted.Fill.insert(lifted.Fill.end(), {a, d, c, a, c, b});
        }
    }
    // An edge between corner rows or columns is outlined where the quads on its two sides differ.
    for (uint32 qz = 0; qz <= quadsZ; ++qz)
        for (uint32 qx = 0; qx < quadsX; ++qx)
            if (IsQuadDrawn(drawn, quadsX, quadsZ, qx, int64(qz) - 1) != IsQuadDrawn(drawn, quadsX, quadsZ, qx, qz))
                lifted.Outline.insert(lifted.Outline.end(),
                                      {GridCorner(grid, stride, qx, qz), GridCorner(grid, stride, qx + 1, qz)});
    for (uint32 qx = 0; qx <= quadsX; ++qx)
        for (uint32 qz = 0; qz < quadsZ; ++qz)
            if (IsQuadDrawn(drawn, quadsX, quadsZ, int64(qx) - 1, qz) != IsQuadDrawn(drawn, quadsX, quadsZ, qx, qz))
                lifted.Outline.insert(lifted.Outline.end(),
                                      {GridCorner(grid, stride, qx, qz), GridCorner(grid, stride, qx, qz + 1)});
}

void DrawLifted(GizmoRenderContext& context, const LiftedSurface& lifted)
{
    if (!lifted.Fill.empty())
        context.DrawTriangles(lifted.Fill.data(), lifted.Fill.size(), kWalkableColor);
    if (lifted.Outline.empty())
        return;
    // The outline's quads face the camera, so they are built every frame; the scratch keeps its
    // capacity. A context with no camera position draws plain lines.
    if (!context.HasCameraWorldPosition())
    {
        context.DrawColoredLines(lifted.Outline.data(), lifted.Outline.size() / 2, kBoundaryColor);
        return;
    }
    static std::vector<Vector3> s_OutlineQuads;
    s_OutlineQuads.clear();
    for (size_t i = 0; i + 1 < lifted.Outline.size(); i += 2)
        AppendThickLineTriangles(context, lifted.Outline[i], lifted.Outline[i + 1], kBoundaryThickness,
                                 /*constantScreenSpaceWidth*/ true, /*smartDistanceScaling*/ false, s_OutlineQuads);
    if (!s_OutlineQuads.empty())
        context.DrawTriangles(s_OutlineQuads.data(), s_OutlineQuads.size(), kBoundaryColor);
}

void DrawNavigationSurface(GizmoRenderContext& context, const ECS::World& world, ECS::EntityHandle entity)
{
    const auto* surface = world.GetComponent<NavigationSurface>(entity);
    if (!surface)
        return;

    LiftedSurface& lifted = Lifted(entity);
    // What the overlay was built from: the map the surface binds and its bake, never an address.
    const uint64 map = (static_cast<uint64>(surface->NavMapIndex) << 32) | surface->NavMapGeneration;
    context.SetTriangleLayer(1); // the transparent layer, drawn over the scene
    if (const auto* mesh = PathfindingECS::ResolveNavigationSurfaceMesh(world, *surface))
    {
        if (!IsCurrent(lifted, {1, map, surface->BakeVersion, mesh->GetDebugMeshVersion()}))
            BuildMesh(lifted, *mesh);
        DrawLifted(context, lifted);
    }
    else if (const auto* grid = PathfindingECS::ResolveNavigationSurfaceGrid(world, *surface))
    {
        // The grid's own version counts topology edits (obstacle stamps) between bakes.
        if (!IsCurrent(lifted, {2, map, surface->BakeVersion, grid->GetTopologyVersion()}))
            BuildGrid(lifted, *grid);
        DrawLifted(context, lifted);
    }
    context.SetTriangleLayer(0);

    if (surface->FitBounds)
        return;
    const auto* transform = world.GetComponent<Components::WorldTransform>(entity);
    const Vector3 position = transform ? Vector3(transform->matrix[12], transform->matrix[13], transform->matrix[14])
                                       : Vector3(0.0f, 0.0f, 0.0f);
    context.DrawWireBox(position + Vector3(surface->Center.x, surface->Center.y, surface->Center.z),
                        Vector3(std::abs(surface->Extent.x), std::abs(surface->Extent.y), std::abs(surface->Extent.z)),
                        kBoundsColor);
}

} // namespace

void RegisterNavigationSurfaceGizmo()
{
    ComponentGizmoRegistry::Get().Register(ECS::GetComponentTypeId<NavigationSurface>(), DrawNavigationSurface);
}

} // namespace GameEngine::Editor::SceneTools
