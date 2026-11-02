#pragma once

#include "GenerationalVector/GenerationalHandle.h"

namespace GameEngine::Physics
{
struct MeshShapeDef;
} // namespace GameEngine::Physics

namespace GameEngine::PhysicsECS
{
/// Immutable CPU geometry for procedural mesh colliders, independent of GPU and physics lifetimes.
/// Register and Release are thread-safe.
/// The registering owner releases the handle after removing every collider that references it.
namespace ProceduralMeshColliders
{
/// Stores the mesh and returns its handle. Returns an invalid handle for fewer than three vertices,
/// no indices, an index count that is not a multiple of three, an index out of range or a
/// non-finite vertex.
GenerationalVector::Handle Register(Physics::MeshShapeDef mesh);
/// Releases the handle. A released handle never resolves to a later mesh.
void Release(GenerationalVector::Handle handle);
} // namespace ProceduralMeshColliders
} // namespace GameEngine::PhysicsECS
