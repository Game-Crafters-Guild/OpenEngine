#pragma once

#include "Components/Rendering/Ocean.h"
#include "Components/Transform.h"

namespace GameEngine::Ocean
{

// The world height of an ocean's calm surface: its entity's world placement (the WorldTransform,
// except while the hierarchy has not resolved it from a Transform yet, Version 0, when the Transform
// is), else the surface's authored SeaLevel for an ocean with no transform.
inline float ResolveOceanSeaLevel(const Components::OceanSurface& ocean, const Components::Transform* transform,
                                  const Components::WorldTransform* worldTransform)
{
    if (worldTransform && (worldTransform->Version != 0u || !transform))
        return worldTransform->matrix[13];
    if (transform)
        return transform->matrix[13];
    return ocean.SeaLevel;
}

} // namespace GameEngine::Ocean
