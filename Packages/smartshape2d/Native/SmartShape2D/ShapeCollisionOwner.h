#pragma once

#include "ECS/ECS.h"
#include "GenerationalVector/GenerationalHandle.h"

namespace GameEngine::ECS { class World; }

namespace GameEngine::SmartShape2D
{
/// Install generated collision on its authored owner while retaining existing physics settings.
/// Returns false if a mesh collider belongs to another geometry provider.
bool SetShapeCollision(ECS::World& world, ECS::EntityHandle owner, GenerationalVector::Handle data);
void RemoveShapeCollision(ECS::World& world, ECS::EntityHandle owner);
}
