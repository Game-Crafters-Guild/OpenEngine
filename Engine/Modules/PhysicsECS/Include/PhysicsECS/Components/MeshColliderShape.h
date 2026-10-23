#pragma once

#include "ECS/RequiredComponents.h"
#include "PhysicsECS/Components/PhysicsCollider.h"
#include "Types/Types.h"

namespace GameEngine::Components
{
/// An immutable triangle mesh supplied by a procedural geometry owner through MeshColliderData.
/// The owner keeps that data alive until this component is removed. Triangle meshes are static.
/// @ge-no-add
/// [DoNotSerialize]
struct MeshColliderShape
{
    uint32 DataIndex = 0;
    uint32 DataGeneration = 0;
    uint32 BuiltDataIndex = 0xFFFFFFFFu;
    uint32 BuiltDataGeneration = 0;
};
}

namespace GameEngine::ECS::Detail
{
template<>
struct RequiredComponents<GameEngine::Components::MeshColliderShape>
{
    using type = TypeList<GameEngine::Components::PhysicsCollider>;
};
}
