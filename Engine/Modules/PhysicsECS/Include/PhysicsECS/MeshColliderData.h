#pragma once

#include "GenerationalVector/GenerationalHandle.h"

#include <memory>

namespace GameEngine::Physics { struct MeshShapeDef; }

namespace GameEngine::PhysicsECS
{
/// Immutable CPU geometry for procedural mesh colliders, independent of GPU and physics lifetimes.
/// Register/Release and Read are thread-safe. A Read result remains valid across Release.
/// The registering owner releases the handle after removing every collider that references it.
namespace MeshColliderData
{
GenerationalVector::Handle Register(Physics::MeshShapeDef mesh);
std::shared_ptr<const Physics::MeshShapeDef> Read(GenerationalVector::Handle handle);
void Release(GenerationalVector::Handle handle);
}
}
