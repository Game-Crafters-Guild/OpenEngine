#pragma once

#include "Types/Types.h"
#include "Components/Transform.h"
#include "ECS/RequiredComponents.h"

#include <type_traits>

namespace GameEngine::Components
{

// An entity that the world's spatial grid indexes as a sphere: a root at its Transform position, a child
// at its WorldTransform position.
// SpatialGridBuildSystem reads it at the start of each simulation step.
struct SpatialGridMover
{
    // @ge-tooltip Meters. The mover is a sphere of this radius; 0 is a point.
    // @ge-range 0
    float32 Radius = 0.5f;
    // @ge-tooltip A query finds the mover when this mask and the query's mask share a bit. The game defines the bits.
    uint32 Mask = 1u;
};

static_assert(std::is_trivially_copyable_v<SpatialGridMover>);
static_assert(std::is_standard_layout_v<SpatialGridMover>);

} // namespace GameEngine::Components

namespace GameEngine::ECS::Detail
{

template <>
struct RequiredComponents<GameEngine::Components::SpatialGridMover>
{
    using type = TypeList<GameEngine::Components::Transform>;
};

} // namespace GameEngine::ECS::Detail
