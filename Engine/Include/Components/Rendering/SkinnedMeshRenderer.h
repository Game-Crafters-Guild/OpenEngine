#pragma once

#include "Types/Types.h"

namespace GameEngine
{
namespace Components
{

struct SkinnedMeshRenderer
{
    uint32 meshId = 0;     // mesh with bone weights
    uint32 skeletonId = 0; // [DoNotSerialize] @ge-readonly process-local cache resolved from SkeletonRef/model source
    bool castShadows = true;
    bool receiveShadows = true;
};

} // namespace Components
} // namespace GameEngine
