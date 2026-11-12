#pragma once

#include "Mathematics/Matrix3x3.h"
#include "Mathematics/WorldPosition.h"
#include "Physics/PhysicsTypes.h"
#include "Types/Types.h"

namespace GameEngine::Components
{
/// What the physics writeback last put on an entity, so it can skip an entity whose inputs have not
/// changed instead of composing a pose only to find the bytes equal. Runtime state: never serialized.
///
/// Valid only when the last composition did not depend on the interpolation alpha (no interpolation,
/// or the previous and current backend poses were bit-identical); see WritePhysicsPose.
struct PhysicsWritebackRecord
{
    /// The backend pose (current, not interpolated) the writeback composed from.
    Physics::Transform AppliedPose{};
    /// The position and basis bytes the writeback wrote to both Transform and WorldTransform.
    Mathematics::WorldPosition WrittenPosition{};
    Mathematics::Matrix3x3 WrittenBasis;
    bool Valid = false;
};

} // namespace GameEngine::Components
