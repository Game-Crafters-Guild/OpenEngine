#pragma once

#include "AssetCore/GUID.h"
#include "Mathematics/Matrix4x4.h"
#include "PathfindingECS/Navigation.h"
#include "TerrainECS/SurfaceHeightQuery.h"
#include "Types/Types.h"

#include <atomic>
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

namespace GameEngine::Rendering
{
class MeshGPURegistry;
} // namespace GameEngine::Rendering

namespace JobSystem
{
class WorkStealingThreadPool;
} // namespace JobSystem

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
// (model submeshes, primitives and generated meshes such as spline chunks) that the entity's
// NavigationSource policy admits. Under Auto,
// the default, a mesh is left out when it or any ancestor has a Dynamic or Kinematic body or a
// navigation agent: they move, so the surface does not bake them. Under an inherited Include a
// mesh is baked where it stands unless it moves on its own below the Include's entity. Gathered
// when a bake starts and never changed afterwards, so tile jobs read it concurrently.
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

// What NavigationStaticGeometry is built from, copied out of the world when a bake starts: the
// collider shapes, already as triangles (they are few and small), and every mesh renderer as its
// world matrix, the index of its mesh and its bake source, each distinct mesh copied once in its own
// space.
// BuildNavigationStaticGeometry reads nothing else, so it runs on any thread.
struct NavigationGeometrySources
{
    struct MeshInstance
    {
        uint32 Mesh = 0;
        Mathematics::Matrix4x4 World;
        Navigation::BakeSource Source;
    };

    NavigationStaticGeometry Shapes;
    // Local positions and triangles, one per distinct mesh.
    std::vector<NavigationTriangles> Meshes;
    // In the order the mesh renderers are gathered.
    std::vector<MeshInstance> Instances;
};

// `meshes` supplies the kept CPU copy of each generated mesh a renderer with no model draws; null
// reads none. The snapshot can run in an ECS wave on a job: it copies the mesh owners it needs under
// one TableScope, then copies their triangles with no lock.
NavigationGeometrySources SnapshotNavigationGeometrySources(ECS::World& world, const Rendering::MeshGPURegistry* meshes);

// The shapes, then every mesh instance transformed to world space as one item, in order. Spreads the
// instances over `pool` (null runs them on the caller). A chunk of instances that starts once `cancelled` is
// set (null: never) is skipped, its vertices and indices left zero and its items' bounds empty: the geometry
// is partial, and the caller that set the flag drops it.
NavigationStaticGeometry BuildNavigationStaticGeometry(NavigationGeometrySources sources,
                                                       JobSystem::WorkStealingThreadPool* pool,
                                                       const std::atomic<bool>* cancelled);

// SnapshotNavigationGeometrySources and BuildNavigationStaticGeometry on the caller.
NavigationStaticGeometry GatherNavigationStaticGeometry(ECS::World& world, const Rendering::MeshGPURegistry* meshes);

// The mesh registry of the engine's renderer, or null when no renderer runs.
const Rendering::MeshGPURegistry* FindEngineMeshRegistry();

// The models the mesh renderers GatherNavigationStaticGeometry reads show that are not resident,
// each once. Primitives and generated meshes have no asset behind them and are never listed.
std::vector<GUID> FindUnloadedNavigationModels(ECS::World& world);

// The terrain a surface bakes: the terrain under the surface's origin, as TerrainECS resolves it.
struct NavigationTerrainSource
{
    // The planar query, or the planet's spherical query (IsSpherical) when the planet is streamed.
    TerrainECS::SurfaceHeightQuery Query;
    // The terrain under the surface's origin is a planet. A planet covers every point but its center.
    bool Spherical = false;
    // The planet's relief shell from its center (the world origin), TerrainECS::PlanetReliefShellOf.
    TerrainECS::PlanetReliefShell PlanetRelief;
    // There is no query and no planet under the origin, and the first terrain is planar: its
    // heightfield has not been built yet.
    bool Loading = false;

    bool OverlapsXZ(float32 minX, float32 minZ, float32 maxX, float32 maxZ) const;
    // True when the terrain is a planet and `bounds` holds a point of its relief: the bake refuses
    // such a surface. Bounds away from the relief read the static geometry only.
    bool ReachesPlanetRelief(const NavigationBounds& bounds) const;
};

// `surfaceTransform` is the navigation surface's transform; null reads the terrain under the world origin.
NavigationTerrainSource ResolveNavigationTerrainSource(ECS::World& world,
                                                       const Components::WorldTransform* surfaceTransform);

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
