#pragma once

#include "ECS/ECS.h"
#include "Types/Types.h"

namespace GameEngine::SpatialGridECS
{

// A grid id puts the entity index above the version, so that ascending ids are ascending entity indices.
inline uint32 ToGridId(ECS::EntityHandle entity)
{
    return (static_cast<uint32>(entity.index) << ECS::kEntityVersionBits) | static_cast<uint32>(entity.version);
}

inline ECS::EntityHandle ToEntity(uint32 gridId)
{
    return ECS::EntityHandle(gridId >> ECS::kEntityVersionBits,
                             static_cast<ECS::EntityVersion>(gridId & ECS::kEntityVersionMask));
}

} // namespace GameEngine::SpatialGridECS
