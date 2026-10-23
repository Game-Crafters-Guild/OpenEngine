#pragma once

#include "Mathematics/Vector3.h"
#include "Types/Types.h"

namespace GameEngine::ECS { class World; }

namespace GameEngine::SmartShape2D
{
/// Cuts a world-XY blast disk through opted-in terrain. Exclusive world access
/// is required. Damage is attached to each shape; its authored spline is retained.
uint32 ExplodeShapes(ECS::World& world, Mathematics::Vector3 center, float radius);
/// Removes explosion footprints and regenerates the authored terrain on its next update.
void ResetShapeDestruction(ECS::World& world);
}
