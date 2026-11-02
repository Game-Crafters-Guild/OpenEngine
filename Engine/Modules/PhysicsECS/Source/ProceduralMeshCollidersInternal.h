#pragma once

#include "GenerationalVector/GenerationalHandle.h"

#include <memory>

namespace GameEngine::Physics
{
struct MeshShapeDef;
} // namespace GameEngine::Physics

namespace GameEngine::PhysicsECS::ProceduralMeshColliders
{
/// The mesh of a live handle; null for a released or invalid handle. Thread-safe. The result
/// stays valid after Release.
std::shared_ptr<const Physics::MeshShapeDef> Read(GenerationalVector::Handle handle);
} // namespace GameEngine::PhysicsECS::ProceduralMeshColliders
