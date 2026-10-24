#pragma once

#include "AssetCore/GUID.h"
#include "PathfindingECS/Navigation.h"
#include "TerrainECS/SurfaceHeightQuery.h"
#include "Types/Types.h"

#include <vector>

namespace GameEngine::ECS
{
class World;
} // namespace GameEngine::ECS

namespace GameEngine::Components
{
struct NavigationSurface;
struct WorldTransform;
} // namespace GameEngine::Components

namespace GameEngine::PathfindingECS
{

// An axis-aligned box in world metres.
struct NavigationBounds
{
    float32 Min[3] = {0.0f, 0.0f, 0.0f};
    float32 Max[3] = {0.0f, 0.0f, 0.0f};
    bool Empty = true;

    void Include(const float32 point[3]);
    void Include(const NavigationBounds& other);
    // True when the XZ rectangles touch.
    bool OverlapsXZ(float32 minX, float32 minZ, float32 maxX, float32 maxZ) const;
};

// World-space triangles: the input a navigation mesh tile or a grid bake reads.
struct NavigationTriangles
{
    std::vector<float32> Vertices; // XYZ interleaved
    std::vector<uint32> Indices;   // three per triangle, into Vertices

    uint32 VertexCount() const { return static_cast<uint32>(Vertices.size() / 3); }
    uint32 TriangleCount() const { return static_cast<uint32>(Indices.size() / 3); }
    void Clear();
};

// The static geometry a surface bakes besides the terrain, in world space: the collider shapes of
// Static physics bodies (box, sphere, capsule, plane) and the meshes of MeshRenderer entities
// (model submeshes and primitives) that the entity's NavigationSource policy admits. Under Auto,
// the default, a mesh is left out when it or any ancestor has a Dynamic or Kinematic body or a
// navigation agent: they move, so the surface does not bake them. Under an inherited Include a
// mesh is baked where it stands unless it moves on its own below the Include's entity. Gathered
// on the main thread when a bake starts and never changed afterwards, so tile jobs read it
// concurrently.
struct NavigationStaticGeometry
{
    // One shape or mesh: a run of Triangles.Indices, its bounds and the entity it came from.
    struct Item
    {
        uint32 FirstIndex = 0;
        uint32 IndexCount = 0;
        NavigationBounds Bounds;
        Navigation::BakeSource Source;
    };

    NavigationTriangles Triangles;
    std::vector<Item> Items;
    NavigationBounds Bounds;

    // Appends the triangles of every item whose bounds touch the XZ rectangle to `out`.
    void AppendOverlapping(float32 minX, float32 minZ, float32 maxX, float32 maxZ, NavigationTriangles& out) const;
    // A hash of the triangles of every item whose bounds touch the XZ rectangle, the same whatever
    // order the items were gathered in: each item's vertex positions are hashed in its own order,
    // then the item hashes are sorted and hashed together.
    uint64 HashOverlapping(float32 minX, float32 minZ, float32 maxX, float32 maxZ) const;
};

NavigationStaticGeometry GatherNavigationStaticGeometry(ECS::World& world);

// The models the mesh renderers GatherNavigationStaticGeometry reads show that are not resident,
// each once. Primitives have no asset behind them and are never listed.
std::vector<GUID> FindUnloadedNavigationModels(ECS::World& world);

// The terrain a surface bakes: the first enabled planar terrain, as TerrainECS resolves it.
struct NavigationTerrainSource
{
    TerrainECS::SurfaceHeightQuery Query;
    // The first terrain is a planet: there is no planar heightfield to read.
    bool Spherical = false;
    // The first terrain is planar but its heightfield has not been built yet.
    bool Loading = false;

    bool OverlapsXZ(float32 minX, float32 minZ, float32 maxX, float32 maxZ) const;
};

NavigationTerrainSource ResolveNavigationTerrainSource(ECS::World& world);

// Appends the terrain's lattice quads touching the XZ rectangle to `out`, taking every
// `stride`-th lattice corner, each corner at the finest height the terrain has (RequireFinest:
// navigation bakes the ground as drawn up close, design page-streaming-planets 3.8). False when a
// corner has no finest answer on the CPU yet: `outWaitPoint` is that corner and `out` is left as it
// was.
bool AppendTerrainLattice(const NavigationTerrainSource& terrain, float32 minX, float32 minZ, float32 maxX,
                          float32 maxZ, uint32 stride, NavigationTriangles& out, Mathematics::Vector3& outWaitPoint);

// The box a surface covers: its explicit bounds around the entity (`transform` may be null), or,
// fitted, the planar terrain's footprint, else everything the bake reads.
NavigationBounds NavigationSurfaceBounds(const Components::NavigationSurface& surface,
                                         const Components::WorldTransform* transform,
                                         const NavigationTerrainSource& terrain,
                                         const NavigationStaticGeometry& geometry);

// Cells of `cellSize` that cover `length` metres: at least one.
uint32 NavigationCellsAlong(float32 length, float32 cellSize);

// What a bake of a surface lays over its bounds: mesh tiles in Mesh mode, grid cells in Grid mode,
// along X and Z. Zero when the cell size is not positive. The one count the bake and
// Navigation::BakeUnitCount share.
struct NavigationBakeExtent
{
    uint32 X = 0;
    uint32 Z = 0;

    uint64 Count() const { return static_cast<uint64>(X) * Z; }
};

NavigationBakeExtent NavigationBakeExtentOf(const Components::NavigationSurface& surface,
                                            const NavigationBounds& bounds);

// Lattice corners to step per vertex so the terrain is not sampled much finer than `cellSize`.
uint32 TerrainLatticeStride(const NavigationTerrainSource& terrain, float32 cellSize);

} // namespace GameEngine::PathfindingECS
