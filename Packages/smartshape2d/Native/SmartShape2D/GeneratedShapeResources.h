#pragma once

#include "AssetCore/GUID.h"
#include "ECS/ECS.h"
#include "GenerationalVector/GenerationalHandle.h"

namespace GameEngine::Components
{
/// Resource ownership on generated children, never authored or serialized.
struct GeneratedShapeResources
{
    ECS::EntityHandle Owner;
    GUID MeshKey;
    uint32 Part = 0xFFFFFFFFu;
    GenerationalVector::Handle CollisionData;
};
}
