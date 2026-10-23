#pragma once

#include "AssetCore/GUID.h"
#include "ECS/ECS.h"
#include "GenerationalVector/GenerationalHandle.h"

namespace GameEngine::Components
{
/// Resource ownership on generated meshes and the shape collider, never serialized.
struct GeneratedShapeResources
{
    ECS::EntityHandle Owner;
    GUID MeshKey;
    uint32 Part = 0xFFFFFFFFu;
    GenerationalVector::Handle CollisionData;
    bool AddedPhysicsBody = false;
    bool AddedPhysicsCollider = false;
};
}
