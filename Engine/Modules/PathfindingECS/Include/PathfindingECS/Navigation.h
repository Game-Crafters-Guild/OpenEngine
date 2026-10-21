#pragma once

#include "ECS/ECS.h"
#include "PathfindingECS/Components/NavigationSource.h"
#include "Types/Types.h"

#include <string>
#include <vector>

namespace GameEngine::ECS
{
class World;
} // namespace GameEngine::ECS

namespace GameEngine::Components
{
struct NavigationSurface;
} // namespace GameEngine::Components

namespace GameEngine::Mathematics
{
struct Vector3;
} // namespace GameEngine::Mathematics

namespace GameEngine::Navigation
{

/// Asks the NavigationSurface on `surface` to bake again from the current terrain, static collider
/// shapes and mesh renderers. The bake starts on the next update of NavigationSurfaceSystem; a mesh
/// keeps its current tiles until each new one is built. Returns false when the entity has no
/// NavigationSurface.
bool RequestBake(ECS::World& world, ECS::EntityHandle surface);

/// What a bake reads from an entity besides the terrain.
enum class BakeSourceKind : uint8
{
    /// A box, sphere, capsule or plane collider on a Static physics body.
    Collider = 0,
    /// The mesh a MeshRenderer shows: a model submesh or a primitive.
    Mesh = 1,
};

/// One source a bake reads. For a mesh, Policy is the NavigationSource policy that admitted it, Auto
/// or Include (an excluded mesh is never listed). A collider keeps its own rule and has no policy:
/// Policy holds Auto.
struct BakeSource
{
    ECS::EntityHandle Entity;
    BakeSourceKind Kind = BakeSourceKind::Collider;
    Components::NavigationSourcePolicy Policy = Components::NavigationSourcePolicy::Auto;
};

/// Lists in `out` every collider and mesh a bake of the NavigationSurface on `surface` reads inside
/// its bounds, each entity once per kind, in the order the bake gathers them. Gathered, so not for
/// every frame. False, with `out` empty, when the entity has no NavigationSurface.
bool ListBakeSources(ECS::World& world, ECS::EntityHandle surface, std::vector<BakeSource>& out);

/// The surface's bake state as one line of text, for example "Baking: 212 of 1,024 tiles" or
/// "Baked: 1,024 tiles, 1,948 polygons, 0.5 MiB, version 12". A failure names what to change.
std::string FormatBakeStatus(const Components::NavigationSurface& surface);

/// The box, in world metres, the NavigationSurface on `surface` covers: its explicit bounds around
/// the entity, or, with Fit Bounds, the planar terrain's footprint, else everything the bake reads
/// (gathered, so not for every frame). False when the entity has no surface or the box is empty.
bool SurfaceBounds(ECS::World& world, ECS::EntityHandle surface, Mathematics::Vector3& outMin,
                   Mathematics::Vector3& outMax);

/// What a bake of `surface` over the box from `min` to `max` makes, as the bake counts it: mesh
/// tiles in Mesh mode, grid cells in Grid mode.
uint64 BakeUnitCount(const Components::NavigationSurface& surface, const Mathematics::Vector3& min,
                     const Mathematics::Vector3& max);

} // namespace GameEngine::Navigation
