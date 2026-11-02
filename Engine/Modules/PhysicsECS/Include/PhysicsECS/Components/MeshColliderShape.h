#pragma once

#include "ECS/RequiredComponents.h"
#include "GenerationalVector/GenerationalHandle.h"
#include "PhysicsECS/Components/PhysicsCollider.h"

#include <type_traits>

namespace GameEngine::Components
{
/// A static triangle mesh collider. The mesh is immutable CPU data that a procedural geometry
/// owner registers with PhysicsECS::ProceduralMeshColliders; the owner keeps the handle alive until
/// this component is removed. The entity is its own body and its PhysicsBody must be Static.
// @ge-no-add  added by the geometry owner that registers the mesh
// [DoNotSerialize] runtime handle; the owner creates it again after a scene load
struct MeshColliderShape
{
    // The ProceduralMeshColliders handle, set by the owner.
    GenerationalVector::Handle Mesh;
    // The handle the current physics shape was built from, written by PhysicsInitSystem.
    GenerationalVector::Handle BuiltMesh;
};

static_assert(std::is_trivially_copyable_v<MeshColliderShape>, "MeshColliderShape must be trivially copyable for ECS storage");
} // namespace GameEngine::Components

namespace GameEngine::ECS::Detail
{
template<>
struct RequiredComponents<GameEngine::Components::MeshColliderShape>
{
    using type = TypeList<GameEngine::Components::PhysicsCollider>;
};
} // namespace GameEngine::ECS::Detail
